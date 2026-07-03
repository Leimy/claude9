#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <auth.h>
#include <bio.h>
#include <thread.h>
#include <9p.h>
#include "json.h"
#include "claude.h"

enum {
	Qroot,
	Qclone,
	Qmodels,
	Qgraph,
	Qgraphlive,
	Qsess,
	Qctl,
	Qprompt,
	Qconv,
	Qmodel,
	Qtokens,
	Qsystem,
	Qusage,
	Qerror,
	Qstream,
	Qthinking,
};

#define QPATH(sid, type)	((uvlong)(sid)<<8 | (type))
#define QSID(path)		((int)((path)>>8))
#define QTYPE(path)		((int)((path)&0xff))

typedef struct Session Session;
struct Session {
	int id;
	char *name;
	char *parent;	/* optional: name of the session that spawned this one, for graph tracking */
	Conv *conv;
	char *lastreply;
	char *lasterror;
	Usage usage;
	int ref;	/* held by users outside the list; guarded by sessionlk */
	int unlinked;	/* removed from session list; guarded by sessionlk */
	QLock lk;
	int busy;	/* prompt round in flight; guarded by lk */
	long lastact;	/* time(0) of last busy transition; guarded by lk */
	/* streaming */
	QLock streamlk;
	Rendez streamrz;
	Sbuf stream;
	int streamdone;
	int streamgen;
	int closed;	/* session destroyed; guarded by streamlk */
	/* auto-continue on max_tokens or tool-loop round cap */
	int autocont;	/* max auto-continue rounds; 0 = disabled */
	Session *next;
};

typedef struct Faux Faux;
struct Faux {
	Session *clone;
	int streamoff;
	int streamgen;		/* -1 = not latched, >=0 = latched generation */
	int streamopengen;	/* s->streamgen at open time */
	int streamidle;		/* stream was idle (done) at open time */
	Req *curreq;		/* in-progress stream/graph read; guarded by streamlk/graphlk */
	int flushing;		/* curreq has been flushed; guarded by streamlk/graphlk */
	/* Qgraph long-poll state; see graphread */
	char *graphbuf;		/* snapshot currently being delivered, or nil */
	int graphlen;
	int graphoff;
	int graphgen;		/* -1 = never delivered; else generation graphbuf is from */
	int grapheof;		/* Qgraph only: EOF already returned on this fid */
};

static Srv clsrv;

static char *apikey;
static char *defmodel = "claude-opus-4-8";
static int defmaxtokens = 16384;
static char *defsysprompt = nil;
static char *skillsdir = nil;
static char *defskills = nil;
static char *namepath = "/mnt/names/name";

static Session *sessions;
static int nextsid;
static QLock sessionlk;

/*
 * graphgen increments whenever the session graph changes in a
 * way a viewer would care about: a session created or
 * destroyed, or a session's busy/parent/model changes.
 * graphread (Qgraph) blocks a reader until graphgen advances
 * past what it last saw, instead of making clients poll on a
 * timer.
 *
 * Always a leaf lock, like skillslk (see its comment): callers
 * elsewhere may be holding sessionlk and/or a session's s->lk
 * when they call bumpgraph, so bumpgraph must never block
 * waiting on those.  graphread enforces the other half of that
 * discipline itself: it always drops graphlk before calling
 * graphtext(), which takes sessionlk and each session's s->lk,
 * so graphlk is never held while waiting on them either.
 */
static QLock graphlk;
static Rendez graphrz;
static int graphgen;

static void
bumpgraph(void)
{
	qlock(&graphlk);
	graphgen++;
	rwakeupall(&graphrz);
	qunlock(&graphlk);
}

/*
 * Guards only the defskills pointer itself (read in newsession
 * and wrsystem, read-and-replaced in doreloadskills).  Kept
 * separate from sessionlk/Session.lk and always used as a leaf
 * lock (never held while waiting on another lock): wrsystem is
 * called with s->lk already held by its caller, and doreload-
 * skills takes sessionlk then each session's lk while walking
 * the list, so reusing either of those for defskills would put
 * sessionlk/s->lk on both sides of two different nesting orders
 * -- a deadlock waiting to happen.  Callers snapshot the string
 * (estrdup) while holding this lock and use the copy afterward,
 * rather than holding it across any other work.
 */
static QLock skillslk;

/*
 * Read all files from a skills directory and return a malloc'd
 * string containing a "Skills" section suitable for appending
 * to the system prompt.  Each file becomes a subsection with
 * its name as heading.  Returns nil if the directory can't be
 * opened or contains no readable files.
 */
static char*
readskills(char *dir)
{
	int dfd, fd, n, i, gotany, dlen;
	Dir *d;
	char *path, *data;
	Fmt f;

	if(dir == nil || dir[0] == '\0')
		return nil;

	dfd = open(dir, OREAD);
	if(dfd < 0)
		return nil;

	fmtstrinit(&f);
	fmtprint(&f,
		"\n\nSkills\n"
		"------\n"
		"The following skill files were loaded at startup from %s.\n"
		"Follow their instructions.\n\n", dir);

	gotany = 0;
	while((n = dirread(dfd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(d[i].qid.type & QTDIR)
				continue;
			path = esmprint("%s/%s", dir, d[i].name);
			fd = open(path, OREAD);
			free(path);
			if(fd < 0)
				continue;
			data = readfile(fd);
			close(fd);
			if(data == nil || data[0] == '\0'){
				free(data);
				continue;
			}
			dlen = strlen(data);
			fmtprint(&f, "### %s\n%s", d[i].name, data);
			if(dlen > 0 && data[dlen-1] != '\n')
				fmtprint(&f, "\n");
			fmtprint(&f, "\n");
			free(data);
			gotany = 1;
		}
		free(d);
	}
	close(dfd);

	if(!gotany){
		free(fmtstrflush(&f));
		return nil;
	}
	return fmtstrflush(&f);
}

static void freesession(Session*);

/*
 * Session lifetime: sessions live on the global list until a
 * hangup unlinks them (s->unlinked).  Any code that uses a
 * session outside sessionlk must hold a reference taken by
 * sessget/sessgetname and drop it with sessput.  The session
 * memory is freed only when it is unlinked and the last
 * reference is gone, so a hangup cannot pull the Session (or
 * its Conv) out from under an in-flight prompt round or a
 * blocked stream reader.
 */
static Session*
sessget(int id)
{
	Session *s;

	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next)
		if(s->id == id)
			break;
	if(s != nil)
		s->ref++;
	qunlock(&sessionlk);
	return s;
}

static Session*
sessgetname(char *name)
{
	Session *s;

	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next)
		if(strcmp(s->name, name) == 0)
			break;
	if(s != nil)
		s->ref++;
	qunlock(&sessionlk);
	return s;
}

static void
sessput(Session *s)
{
	int dofree;

	if(s == nil)
		return;
	qlock(&sessionlk);
	s->ref--;
	dofree = s->unlinked && s->ref == 0;
	qunlock(&sessionlk);
	if(dofree)
		freesession(s);
}

/*
 * Try to get a name from namefs.  Returns a malloc'd
 * string or nil if the name server is unavailable.
 */
static char*
genname(void)
{
	int fd, n;
	char buf[64];

	fd = open(namepath, OREAD);
	if(fd < 0)
		return nil;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return nil;
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';
	if(n == 0)
		return nil;
	return estrdup(buf);
}

/*
 * Create a session and link it onto the global list.
 * Returns with one reference held for the caller.
 */
static Session*
newsession(void)
{
	Session *s;
	char *name, *skills;
	char buf[32];

	qlock(&skillslk);
	skills = defskills != nil ? estrdup(defskills) : nil;
	qunlock(&skillslk);

	s = emallocz(sizeof *s, 1);
	qlock(&sessionlk);
	s->id = nextsid++;
	s->ref = 1;
	/*
	 * Try namefs for a goofy name; on collision or
	 * failure, fall back to the integer id.
	 */
	name = genname();
	if(name != nil){
		Session *dup;
		for(dup = sessions; dup != nil; dup = dup->next)
			if(strcmp(dup->name, name) == 0)
				break;
		if(dup != nil){
			free(name);
			name = nil;
		}
	}
	if(name == nil){
		snprint(buf, sizeof buf, "%d", s->id);
		name = estrdup(buf);
	}
	s->name = name;
	s->lastact = time(0);
	s->conv = convnew(apikey, defmodel, defmaxtokens, defsysprompt, skills);
	s->streamrz.l = &s->streamlk;
	s->streamdone = 1;
	s->next = sessions;
	sessions = s;
	qunlock(&sessionlk);
	free(skills);
	bumpgraph();
	return s;
}

static void
freesession(Session *s)
{
	if(s == nil)
		return;
	convfree(s->conv);
	free(s->lastreply);
	free(s->lasterror);
	free(s->name);
	free(s->parent);
	free(s->usage.stop_reason);
	free(s->stream.s);
	free(s);
}

/*
 * Unlink a session from the global list.  The memory is
 * reclaimed by sessput when the last reference drops.
 * Blocked stream readers are woken so they see EOF instead
 * of sleeping forever on a dead session.
 */
static void
delsession(int id)
{
	Session **pp, *s;
	int dofree;

	qlock(&sessionlk);
	for(pp = &sessions; *pp != nil; pp = &(*pp)->next){
		if((*pp)->id == id){
			s = *pp;
			*pp = s->next;
			s->next = nil;
			s->unlinked = 1;
			dofree = s->ref == 0;
			qunlock(&sessionlk);
			if(dofree){
				/* no users left; reclaim now */
				freesession(s);
				bumpgraph();
				return;
			}
			qlock(&s->streamlk);
			s->closed = 1;
			rwakeupall(&s->streamrz);
			qunlock(&s->streamlk);
			bumpgraph();
			return;
		}
	}
	qunlock(&sessionlk);
}

/*
 * Streaming helpers.  Text deltas from claudeconverse are
 * appended to s->stream; blocked readers on Qstream are woken
 * via s->streamrz.  All state guarded by s->streamlk.
 *
 * streamclear empties the buffer and bumps the generation;
 * done says whether the session is idle or a round is starting.
 */
static void
streamclear(Session *s, int done)
{
	qlock(&s->streamlk);
	s->stream.len = 0;
	if(s->stream.s != nil)
		s->stream.s[0] = '\0';
	s->streamdone = done;
	s->streamgen++;
	rwakeupall(&s->streamrz);
	qunlock(&s->streamlk);
}

static void
streamappend(Session *s, char *data, int n)
{
	qlock(&s->streamlk);
	sbappend(&s->stream, data, n);
	rwakeupall(&s->streamrz);
	qunlock(&s->streamlk);
}

static void
streamfinish(Session *s)
{
	qlock(&s->streamlk);
	s->streamdone = 1;
	rwakeupall(&s->streamrz);
	qunlock(&s->streamlk);
}

/*
 * Callback invoked by claudeconverse with each incremental
 * text chunk (either SSE text_delta text or an internal
 * "[running ...]" marker between tool rounds).
 */
static void
streamcb(char *chunk, void *aux)
{
	Session *s;
	s = aux;
	streamappend(s, chunk, strlen(chunk));
}

/*
 * Session file table: maps file names to qid types, modes,
 * and I/O handlers.  rd returns the file's contents as a
 * malloc'd string; wr applies a write and returns nil or an
 * error string.  Both are called with s->lk held (and, for
 * wr, s->busy already checked).  Files with rd == nil
 * (stream) or that need to block or act outside the lock
 * (prompt and ctl writes) are special-cased in fsread and
 * fswrite.
 */
static char* rdctl(Session*);
static char* rdprompt(Session*);
static char* rdconv(Session*);
static char* rdmodel(Session*);
static char* rdtokens(Session*);
static char* rdthinking(Session*);
static char* rdsystem(Session*);
static char* rdusage(Session*);
static char* rderror(Session*);
static char* wrmodel(Session*, char*);
static char* wrtokens(Session*, char*);
static char* wrthinking(Session*, char*);
static char* wrsystem(Session*, char*);

static struct {
	char *name;
	int type;
	int mode;
	char* (*rd)(Session*);
	char* (*wr)(Session*, char*);
} sessfiles[] = {
	{ "ctl",	Qctl,		0666,	rdctl,		nil },
	{ "prompt",	Qprompt,	0666,	rdprompt,	nil },
	{ "conv",	Qconv,		0444,	rdconv,		nil },
	{ "model",	Qmodel,		0666,	rdmodel,	wrmodel },
	{ "tokens",	Qtokens,	0666,	rdtokens,	wrtokens },
	{ "system",	Qsystem,	0666,	rdsystem,	wrsystem },
	{ "usage",	Qusage,		0444,	rdusage,	nil },
	{ "error",	Qerror,		0444,	rderror,	nil },
	{ "stream",	Qstream,	0444,	nil,		nil },
	{ "thinking",	Qthinking,	0666,	rdthinking,	wrthinking },
};

/*
 * Root file table, likewise shared by walk, stat, and dirread.
 */
static struct {
	char *name;
	int type;
} rootfiles[] = {
	{ "clone",	Qclone },
	{ "models",	Qmodels },
	{ "graph",	Qgraph },
	{ "graphlive",	Qgraphlive },
};

static void
filldir(Dir *d, Qid qid, char *name, ulong mode)
{
	d->qid = qid;
	d->mode = mode;
	d->atime = d->mtime = time(0);
	d->length = 0;
	d->name = estrdup9p(name);
	d->uid = estrdup9p("claude");
	d->gid = estrdup9p("claude");
	d->muid = estrdup9p("");
}

static int
rootgen(int i, Dir *d, void *v)
{
	Session *s;
	int n;

	USED(v);
	if(i < nelem(rootfiles)){
		filldir(d, (Qid){rootfiles[i].type, 0, QTFILE},
			rootfiles[i].name, 0444);
		return 0;
	}
	i -= nelem(rootfiles);
	qlock(&sessionlk);
	n = 0;
	for(s = sessions; s != nil; s = s->next){
		if(n == i){
			filldir(d, (Qid){QPATH(s->id, Qsess), 0, QTDIR},
				s->name, DMDIR|0555);
			qunlock(&sessionlk);
			return 0;
		}
		n++;
	}
	qunlock(&sessionlk);
	return -1;
}

static int
sessgen(int i, Dir *d, void *aux)
{
	int sid;
	Session *s;

	sid = (int)(uintptr)aux;
	s = sessget(sid);
	if(s == nil)
		return -1;
	sessput(s);
	if(i < 0 || i >= nelem(sessfiles))
		return -1;
	filldir(d, (Qid){QPATH(sid, sessfiles[i].type), 0, QTFILE},
		sessfiles[i].name, sessfiles[i].mode);
	return 0;
}

static char*
convtext(Conv *c)
{
	Msg *m;
	Fmt f;

	fmtstrinit(&f);
	for(m = c->msgs; m != nil; m = m->next)
		fmtprint(&f, "[%s]\n%s\n\n",
			m->role == Muser ? "user" : "assistant", m->text);
	return fmtstrflush(&f);
}

/*
 * One line per live session: name, model, busy flag, parent
 * (the name of whatever session's ctl wrote "parent <name>" to
 * this one, or "-" if none), and idle seconds (0 while busy;
 * otherwise seconds since the last busy transition, i.e. since
 * the last prompt round started or ended -- a freshly created
 * session counts from creation).  Tab-separated so a viewer can
 * enumerate the whole session graph with a single read instead
 * of walking the directory and opening each session's ctl file.
 * Parent is just a label recorded by convention (see the ctl
 * "parent" command) -- claude9fs does not itself create sessions
 * on another session's behalf, so it cannot know the relationship
 * except by being told.
 *
 * The idle field is a point-in-time value: it is NOT re-bumped
 * as time passes (that would defeat the graphlive long-poll).
 * A viewer that wants a live idle age should add its own clock
 * time elapsed since it read the snapshot, which is exactly
 * what claudegraph does when fading long-idle sessions.
 */
static char*
graphtext(void)
{
	Session *s;
	Fmt f;
	long now;

	now = time(0);
	fmtstrinit(&f);
	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next){
		qlock(&s->lk);
		fmtprint(&f, "%s\t%s\t%d\t%s\t%ld\n",
			s->name,
			s->conv->model,
			s->busy,
			s->parent != nil && s->parent[0] != '\0' ? s->parent : "-",
			s->busy ? 0L : now - s->lastact);
		qunlock(&s->lk);
	}
	qunlock(&sessionlk);
	return fmtstrflush(&f);
}

static char*
rdusage(Session *s)
{
	return esmprint(
		"input_tokens %d\n"
		"output_tokens %d\n"
		"total_tokens %d\n"
		"cache_creation_input_tokens %d\n"
		"cache_read_input_tokens %d\n"
		"stop_reason %s\n",
		s->usage.input_tokens,
		s->usage.output_tokens,
		s->usage.input_tokens + s->usage.output_tokens,
		s->usage.cache_creation_input_tokens,
		s->usage.cache_read_input_tokens,
		s->usage.stop_reason ? s->usage.stop_reason : "none");
}

/*
 * Format the session's thinking setting as text:
 *   "0"                  off
 *   "<n>"                budget mode, n tokens (opus etc.)
 *   "adaptive"           adaptive mode (fable)
 *   "adaptive <effort>"  adaptive mode with output effort
 * The same syntax is accepted by writes to the thinking file.
 */
static char*
thinkingtext(Conv *c)
{
	switch(c->thinkmode){
	case Thinkbudget:
		return esmprint("%d", c->thinking);
	case Thinkadaptive:
		if(c->effort != nil && c->effort[0] != '\0')
			return esmprint("adaptive %s", c->effort);
		return estrdup("adaptive");
	}
	return estrdup("0");
}

/*
 * Parse a write to the thinking file.
 *
 * Budget mode enforces the API's invariant up front:
 * 1024 <= budget < maxtokens.  Thinking tokens come out of
 * the same max_tokens output budget as the visible reply, so
 * a budget at or above maxtokens would leave no room for an
 * answer (and the API rejects it).  buildreq passes the value
 * through unchecked, so the invariant must hold here.
 */
static char*
wrthinking(Session *s, char *data)
{
	Conv *c;
	char *p;
	int n;

	c = s->conv;
	while(*data == ' ' || *data == '\t')
		data++;
	if(strcmp(data, "off") == 0 || strcmp(data, "0") == 0){
		c->thinkmode = Thinkoff;
		c->thinking = 0;
		free(c->effort);
		c->effort = nil;
		return nil;
	}
	if(strncmp(data, "adaptive", 8) == 0
	&& (data[8] == '\0' || data[8] == ' ' || data[8] == '\t')){
		p = data + 8;
		while(*p == ' ' || *p == '\t')
			p++;
		c->thinkmode = Thinkadaptive;
		c->thinking = 0;
		free(c->effort);
		c->effort = *p != '\0' ? estrdup(p) : nil;
		return nil;
	}
	if(data[0] >= '0' && data[0] <= '9'){
		n = atoi(data);
		if(n < 1024)
			return "thinking budget must be at least 1024 tokens";
		if(n >= c->maxtokens)
			return "thinking budget must be less than max tokens (see tokens file)";
		c->thinkmode = Thinkbudget;
		c->thinking = n;
		free(c->effort);
		c->effort = nil;
		return nil;
	}
	return "usage: 0 | off | <budget-tokens> | adaptive [effort]";
}

static char*
rdctl(Session *s)
{
	char *think, *text;
	Msg *m;
	long nmsg, nbytes;
	int nexch;

	nmsg = 0;
	for(m = s->conv->msgs; m != nil; m = m->next)
		nmsg++;
	/*
	 * bytes is the approximate input size sent on the next
	 * request: every message's text plus its raw content
	 * array (tool_use/tool_result blocks, which dominate a
	 * long tool-using session).  exchanges is the number of
	 * real user turns -- the unit the compact ctl command
	 * drops.  Together they let a client see history growth
	 * and decide when (and how hard) to compact.
	 */
	nbytes = convinputbytes(s->conv);
	nexch = convnexchanges(s->conv);
	think = thinkingtext(s->conv);
	text = esmprint(
		"name %s\n"
		"parent %s\n"
		"model %s\n"
		"tokens %d\n"
		"messages %ld\n"
		"exchanges %d\n"
		"bytes %ld\n"
		"busy %d\n"
		"autocontinue %d\n"
		"thinking %s\n",
		s->name,
		s->parent != nil && s->parent[0] != '\0' ? s->parent : "-",
		s->conv->model,
		s->conv->maxtokens,
		nmsg,
		nexch,
		nbytes,
		s->busy,
		s->autocont,
		think);
	free(think);
	return text;
}

static char*
rdprompt(Session *s)
{
	return estrdup(s->lastreply ? s->lastreply : "");
}

static char*
rdconv(Session *s)
{
	return convtext(s->conv);
}

static char*
rdmodel(Session *s)
{
	return estrdup(s->conv->model);
}

static char*
rdtokens(Session *s)
{
	return esmprint("%d", s->conv->maxtokens);
}

static char*
rdthinking(Session *s)
{
	return thinkingtext(s->conv);
}

static char*
rdsystem(Session *s)
{
	return estrdup(s->conv->sysprompt ? s->conv->sysprompt : "");
}

static char*
rderror(Session *s)
{
	return estrdup(s->lasterror ? s->lasterror : "");
}

static char*
wrmodel(Session *s, char *data)
{
	free(s->conv->model);
	s->conv->model = estrdup(data);
	bumpgraph();
	return nil;
}

static char*
wrtokens(Session *s, char *data)
{
	int n;

	n = atoi(data);
	if(n <= 0)
		return "invalid token count";
	/* keep the thinking-budget invariant (see wrthinking) */
	if(s->conv->thinkmode == Thinkbudget && n <= s->conv->thinking)
		return "tokens must exceed thinking budget (see thinking file)";
	s->conv->maxtokens = n;
	return nil;
}

static char*
wrsystem(Session *s, char *data)
{
	char *skills;

	qlock(&skillslk);
	skills = defskills != nil ? estrdup(defskills) : nil;
	qunlock(&skillslk);
	convsetprompt(s->conv, data, skills);
	free(skills);
	return nil;
}

/*
 * Re-read the skills directory and apply the result to every
 * live session (each session's own base prompt is preserved;
 * only the skills suffix changes), then update defskills so
 * new sessions pick it up too.  Assumes skillsdir != nil --
 * handlectl checks that synchronously before scheduling a call
 * here, so the ctl write gets a proper error if claude9fs was
 * started without -K.
 *
 * A session with a prompt round in flight is left alone: its
 * conv is safe to touch (claudeconverse does not hold s->lk
 * during the round), but skipping it avoids racing a reply
 * that's using the old skills text mid-round, and "busy" is
 * already this codebase's convention for "don't touch this
 * session's settings right now" (see the busy checks in
 * fswrite).  It simply keeps the old skills text until a later
 * reload finds it idle.
 *
 * Called from fswrite with no session lock held (see the
 * Qctl/reloadskills handling below for why).
 */
static void
doreloadskills(void)
{
	char *new;
	Session *s;

	new = readskills(skillsdir);	/* nil is fine: means no skills */

	/*
	 * Apply to every live session first (each gets its own
	 * estrdup'd copy via convsetprompt; new itself is only
	 * read here, never stored), then publish to defskills for
	 * future sessions.  sessionlk/s->lk nesting here never
	 * involves skillslk, and the skillslk critical section
	 * below never involves sessionlk/s->lk -- see the comment
	 * on skillslk's declaration for why that separation matters.
	 */
	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next){
		qlock(&s->lk);
		if(!s->busy)
			convsetprompt(s->conv, s->conv->basesys, new);
		qunlock(&s->lk);
	}
	qunlock(&sessionlk);

	qlock(&skillslk);
	free(defskills);
	defskills = new;
	qunlock(&skillslk);
}

static void
fsattach(Req *r)
{
	r->fid->qid = (Qid){Qroot, 0, QTDIR};
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	uvlong path;
	int type, i;
	Session *s;

	path = fid->qid.path;
	type = QTYPE(path);

	if(path == Qroot){
		for(i = 0; i < nelem(rootfiles); i++){
			if(strcmp(name, rootfiles[i].name) == 0){
				*qid = (Qid){rootfiles[i].type, 0, QTFILE};
				return nil;
			}
		}
		s = sessgetname(name);
		if(s == nil)
			return "not found";
		*qid = (Qid){QPATH(s->id, Qsess), 0, QTDIR};
		sessput(s);
		return nil;
	}
	if(type == Qsess){
		int sid;
		sid = QSID(path);
		s = sessget(sid);
		if(s == nil)
			return "session gone";
		sessput(s);
		for(i = 0; i < nelem(sessfiles); i++){
			if(strcmp(name, sessfiles[i].name) == 0){
				*qid = (Qid){QPATH(sid, sessfiles[i].type), 0, QTFILE};
				return nil;
			}
		}
		return "not found";
	}
	return "walk in non-directory";
}

static void
fsstat(Req *r)
{
	uvlong path;
	int type, sid, i;
	Session *s;

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	if(path == Qroot){
		filldir(&r->d, (Qid){Qroot, 0, QTDIR}, "/", DMDIR|0555);
		respond(r, nil);
		return;
	}
	for(i = 0; i < nelem(rootfiles); i++){
		if(path == rootfiles[i].type){
			filldir(&r->d, r->fid->qid, rootfiles[i].name, 0444);
			respond(r, nil);
			return;
		}
	}
	if(type == Qsess){
		s = sessget(sid);
		if(s == nil){
			respond(r, "session gone");
			return;
		}
		filldir(&r->d, r->fid->qid, s->name, DMDIR|0555);
		sessput(s);
		respond(r, nil);
		return;
	}

	/* session file -- look up in table */
	s = sessget(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}
	sessput(s);
	for(i = 0; i < nelem(sessfiles); i++){
		if(sessfiles[i].type == type){
			filldir(&r->d, r->fid->qid,
				sessfiles[i].name, sessfiles[i].mode);
			respond(r, nil);
			return;
		}
	}
	respond(r, "unknown file");
}

static void
modelstext(Req *r)
{
	char *buf;

	buf = fetchmodels(apikey);
	if(buf == nil)
		buf = esmprint("error fetching models: %r\n");
	readstr(r, buf);
	free(buf);
	respond(r, nil);
}

/*
 * Blocking read on Qstream.  Returns new bytes past fa->streamoff,
 * or EOF when the current round's stream is done, or blocks
 * waiting for data / end-of-round / start-of-next-round.
 *
 * A newly-opened fid whose session was idle at open time
 * blocks until the next round starts.  To see the last round's
 * text after the fact, read the prompt file instead.
 *
 * While blocked, the request is registered in fa->curreq so
 * fsflush can cancel it; a hangup of the session sets s->closed
 * and wakes us so we return EOF instead of sleeping forever.
 * (If a client somehow issues multiple concurrent reads on one
 * fid, only the latest is flushable; the others still wake and
 * terminate normally.)
 */
static void
streamread(Req *r, Session *s, Faux *fa)
{
	long avail, want;

	qlock(&s->streamlk);
	fa->curreq = r;
	fa->flushing = 0;
	/*
	 * On first read, latch to the current generation.
	 * If the session was idle at open time, wait for the
	 * next round to start first.
	 */
	if(fa->streamgen < 0){
		if(fa->streamidle){
			while(s->streamgen == fa->streamopengen
			   && s->streamdone && !s->closed && !fa->flushing)
				rsleep(&s->streamrz);
		}
		fa->streamgen = s->streamgen;
	}
	for(;;){
		if(fa->flushing){
			fa->curreq = nil;
			fa->flushing = 0;
			qunlock(&s->streamlk);
			respond(r, "interrupted");
			return;
		}
		/* session hung up, or fid's generation retired -- EOF */
		if(s->closed || fa->streamgen != s->streamgen){
			fa->curreq = nil;
			qunlock(&s->streamlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if(fa->streamoff < s->stream.len){
			avail = s->stream.len - fa->streamoff;
			want = r->ifcall.count;
			if(want > avail) want = avail;
			memmove(r->ofcall.data, s->stream.s + fa->streamoff, want);
			r->ofcall.count = want;
			fa->streamoff += want;
			fa->curreq = nil;
			qunlock(&s->streamlk);
			respond(r, nil);
			return;
		}
		if(s->streamdone){
			fa->curreq = nil;
			qunlock(&s->streamlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		rsleep(&s->streamrz);
	}
}

/*
 * Two files expose the session graph, differing only in what
 * happens once a fid has delivered the whole current snapshot:
 *
 *   graph      A plain, always-terminating snapshot.  Reads
 *              hand back the current snapshot; once it is fully
 *              delivered, the next read returns EOF (count 0).
 *              Safe for any read-to-EOF consumer -- cat(1), a
 *              shell $(), an agent's read_file tool -- none of
 *              which would ever expect a read to block forever.
 *              This is the file to poke by hand or from a script.
 *
 *   graphlive  A blocking long-poll for a live viewer.  The
 *              first read returns the current snapshot; once it
 *              is fully delivered, the next read BLOCKS until
 *              the graph changes (a session created/destroyed,
 *              or a busy/parent/model change -- see bumpgraph's
 *              callers), then returns the new snapshot.  A
 *              viewer opens this once and sits in a plain read
 *              loop, redrawing whatever comes back, with no
 *              polling timer.  claudegraph uses this.
 *
 * In both cases each fid's notion of "current" is private and
 * independent of the requested offset, and a short read (fewer
 * bytes than asked for) is NOT EOF: if the client's buffer is
 * smaller than one snapshot, read again on the same fid to get
 * the rest of the snapshot before the fid either returns EOF
 * (graph) or blocks for the next change (graphlive).
 *
 * fa->graphgen/graphbuf/graphlen/graphoff are private per-fid
 * state guarded by graphlk, set up by fsopen and released by
 * fsdestroyfid.  graphread implements both files; the caller
 * passes live=1 for graphlive, live=0 for graph.  fa->grapheof
 * latches once a non-live fid has returned EOF, so a subsequent
 * read stays at EOF instead of re-fetching the same snapshot.
 */
static void
graphread(Req *r, Faux *fa, int live)
{
	long avail, want;
	char *text;

	qlock(&graphlk);
	fa->curreq = r;
	fa->flushing = 0;
	for(;;){
		if(fa->flushing){
			fa->curreq = nil;
			fa->flushing = 0;
			qunlock(&graphlk);
			respond(r, "interrupted");
			return;
		}
		if(fa->graphbuf != nil && fa->graphoff < fa->graphlen)
			break;	/* more of the current snapshot to deliver */
		if(!live && fa->grapheof){
			/* already signalled EOF on this fid; stay there */
			fa->curreq = nil;
			qunlock(&graphlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if(fa->graphgen >= 0 && fa->graphgen == graphgen){
			if(!live){
				/*
				 * Snapshot fully delivered and nothing has
				 * changed: a plain file returns EOF rather
				 * than blocking, so read-to-EOF consumers
				 * terminate.  Latch it so re-reads stay at
				 * EOF instead of re-fetching the same text.
				 */
				fa->grapheof = 1;
				fa->curreq = nil;
				qunlock(&graphlk);
				r->ofcall.count = 0;
				respond(r, nil);
				return;
			}
			rsleep(&graphrz);	/* graphlive: wait for a change */
			continue;
		}
		/*
		 * First read ever on this fid (graphgen < 0), or the
		 * graph changed since our last snapshot: fetch a fresh
		 * one.  graphtext() takes sessionlk and per-session
		 * locks, so graphlk must not be held here -- see its
		 * declaration comment.
		 */
		qunlock(&graphlk);
		text = graphtext();
		qlock(&graphlk);
		free(fa->graphbuf);
		fa->graphbuf = text;
		fa->graphlen = strlen(text);
		fa->graphoff = 0;
		fa->graphgen = graphgen;
	}
	avail = fa->graphlen - fa->graphoff;
	want = r->ifcall.count;
	if(want > avail)
		want = avail;
	memmove(r->ofcall.data, fa->graphbuf + fa->graphoff, want);
	r->ofcall.count = want;
	fa->graphoff += want;
	fa->curreq = nil;
	qunlock(&graphlk);
	respond(r, nil);
}

/*
 * Tflush: if the flushed request is a stream or graph read
 * blocked in streamread/graphread, mark it and wake the
 * sleeper; it responds to the old request with "interrupted".
 * For anything else (e.g. an in-flight prompt write, which
 * cannot be cancelled mid-API call) we just respond to the
 * flush; lib9p delays the Rflush until the old request's
 * response is sent.
 */
static void
fsflush(Req *r)
{
	Req *old;
	Faux *fa;
	Session *s;
	uvlong path;

	old = r->oldreq;
	if(old != nil && old->fid != nil){
		path = old->fid->qid.path;
		if(QTYPE(path) == Qstream){
			fa = old->fid->aux;
			s = sessget(QSID(path));
			if(s != nil && fa != nil){
				qlock(&s->streamlk);
				if(fa->curreq == old){
					fa->flushing = 1;
					rwakeupall(&s->streamrz);
				}
				qunlock(&s->streamlk);
			}
			sessput(s);
		} else if(path == Qgraph || path == Qgraphlive){
			fa = old->fid->aux;
			if(fa != nil){
				qlock(&graphlk);
				if(fa->curreq == old){
					fa->flushing = 1;
					rwakeupall(&graphrz);
				}
				qunlock(&graphlk);
			}
		}
	}
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	Faux *fa;
	uvlong path;
	int type, sid;
	Session *s;

	fa = r->fid->aux;
	if(fa == nil){
		fa = emallocz(sizeof *fa, 1);
		r->fid->aux = fa;
	}

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	fa->streamgen = -1;	/* not yet latched */
	if(type == Qstream){
		s = sessget(sid);
		if(s != nil){
			qlock(&s->streamlk);
			fa->streamopengen = s->streamgen;
			fa->streamidle = s->streamdone;
			qunlock(&s->streamlk);
			sessput(s);
		}
	}
	/* Qgraph: forget any snapshot from a previous open of this fid */
	free(fa->graphbuf);
	fa->graphbuf = nil;
	fa->graphlen = 0;
	fa->graphoff = 0;
	fa->graphgen = -1;
	fa->grapheof = 0;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	uvlong path;
	int type, sid, i;
	Session *s;
	char *text;
	Faux *fa;

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	fa = r->fid->aux;	/* always set by fsopen */

	if(path == Qroot){
		dirread9p(r, rootgen, nil);
		respond(r, nil);
		return;
	}

	if(path == Qclone){
		if(fa->clone == nil)
			fa->clone = newsession();
		s = fa->clone;
		readstr(r, s->name);
		respond(r, nil);
		return;
	}

	if(path == Qmodels){
		srvrelease(&clsrv);
		modelstext(r);
		srvacquire(&clsrv);
		return;
	}

	if(path == Qgraph || path == Qgraphlive){
		srvrelease(&clsrv);
		graphread(r, fa, path == Qgraphlive);
		srvacquire(&clsrv);
		return;
	}

	if(type == Qsess){
		dirread9p(r, sessgen, (void*)(uintptr)sid);
		respond(r, nil);
		return;
	}

	s = sessget(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}

	if(type == Qstream){
		srvrelease(&clsrv);
		streamread(r, s, fa);
		srvacquire(&clsrv);
		sessput(s);
		return;
	}

	for(i = 0; i < nelem(sessfiles); i++)
		if(sessfiles[i].type == type && sessfiles[i].rd != nil)
			break;
	if(i == nelem(sessfiles)){
		respond(r, "bug in fsread");
		sessput(s);
		return;
	}
	qlock(&s->lk);
	text = sessfiles[i].rd(s);
	qunlock(&s->lk);
	readstr(r, text);
	free(text);
	respond(r, nil);
	sessput(s);
}

/*
 * Handle a ctl command.  Called with s->lk held.
 * Returns nil on success or an error string.
 *
 * hangupp/reloadp are out-parameters for the two commands whose
 * real work must happen with no session lock held: hangup acts
 * on the whole session (including freeing it), and reloadskills
 * acts on every session, including this one, which is already
 * locked by our caller (fswrite) -- relocking it here would
 * deadlock.  Both commands just flag the request and return;
 * fswrite does the actual work after unlocking.
 */
static char*
handlectl(Session *s, char *cmd, int *hangupp, int *reloadp)
{
	if(strcmp(cmd, "clear") == 0){
		if(s->busy)
			return "session busy";
		convclear(s->conv);
		free(s->lastreply);
		s->lastreply = nil;
		free(s->lasterror);
		s->lasterror = nil;
		free(s->usage.stop_reason);
		memset(&s->usage, 0, sizeof s->usage);
		streamclear(s, 1);
	} else if(strncmp(cmd, "compact", 7) == 0
	&& (cmd[7] == '\0' || cmd[7] == ' ')){
		char *v;
		int keep;
		if(s->busy)
			return "session busy";
		v = cmd + 7;
		while(*v == ' ') v++;
		/*
		 * keep = number of most recent exchanges to retain.
		 * Default 4: enough recent context to keep working,
		 * while shedding the bulk of an old session (each
		 * exchange can carry tens of KB of stale tool output).
		 * The most recent exchange is never dropped.  The
		 * effect is visible in ctl's exchanges/bytes fields.
		 */
		if(*v == '\0')
			keep = 4;
		else
			keep = atoi(v);
		if(keep < 1)
			return "compact: keep count must be at least 1";
		convcompact(s->conv, keep);
	} else if(strncmp(cmd, "parent", 6) == 0
	&& (cmd[6] == '\0' || cmd[6] == ' ')){
		/*
		 * Record which session (by name) spawned this one, for
		 * a graph viewer to draw an edge.  This is purely a
		 * label the caller volunteers -- typically right after
		 * cloning a sub-agent session -- claude9fs has no other
		 * way to know the relationship.  "parent -" or "parent"
		 * with nothing after it clears the label.
		 */
		char *v;
		v = cmd + 6;
		while(*v == ' ') v++;
		free(s->parent);
		s->parent = (*v != '\0' && strcmp(v, "-") != 0) ? estrdup(v) : nil;
		bumpgraph();
	} else if(strcmp(cmd, "hangup") == 0){
		*hangupp = 1;
	} else if(strcmp(cmd, "reloadskills") == 0){
		if(skillsdir == nil)
			return "no skills directory configured (start claude9fs with -K)";
		*reloadp = 1;
	} else if(strncmp(cmd, "autocontinue", 12) == 0){
		char *v;
		int n;
		v = cmd + 12;
		while(*v == ' ') v++;
		if(*v == '\0')
			n = 3;	/* default: up to 3 rounds */
		else
			n = atoi(v);
		if(n < 0) n = 0;
		s->autocont = n;
	} else if(strcmp(cmd, "noautocontinue") == 0){
		s->autocont = 0;
	} else
		return "unknown ctl command";
	return nil;
}

/*
 * Run one prompt round (plus auto-continue rounds) for a
 * session.  Called from fswrite with the srv loop released
 * and a session reference held.  s->busy is set, so no other
 * prompt write, clear, or model/system/tokens change can
 * touch s->conv while claudeconverse is walking it.
 *
 * Round 0 answers the user's message; if a round was
 * guillotined by max_tokens, or hit the tool-loop round cap
 * while still calling tools, and auto-continue is enabled,
 * "Continue." is sent up to autocont more times.  The stream
 * stays open across continuations, so readers see seamless
 * text.  A genuinely failed round stops the loop: continuing a
 * broken round compounds the damage and hides the error.  The
 * tool-loop-limit case is not such a failure -- the
 * conversation is left well-formed and resumable -- so it is
 * treated like max_tokens (see the loop below).
 *
 * Token usage accumulates in a local Usage and is published
 * to s->usage under s->lk only when the round ends, so other
 * fids can read the usage file at any time without racing the
 * unlocked updates claudeconverse makes mid-round.  (Mid-round
 * reads see the previous round's totals.)
 */
static void
doprompt(Req *r, Session *s, char *data)
{
	char *reply, *err;
	int autocont, round, ok;
	Usage u;
	Fmt f;

	qlock(&s->lk);
	if(s->busy){
		qunlock(&s->lk);
		free(data);
		respond(r, "session busy");
		return;
	}
	s->busy = 1;
	s->lastact = time(0);
	free(s->lasterror);
	s->lasterror = nil;
	convappend(s->conv, msgnew(Muser, data, nil));
	free(data);
	autocont = s->autocont;
	qunlock(&s->lk);
	bumpgraph();

	memset(&u, 0, sizeof u);
	streamclear(s, 0);
	fmtstrinit(&f);
	ok = 0;
	err = nil;

	for(round = 0; ; round++){
		reply = claudeconverse(s->conv, &u, streamcb, s, &err);
		if(reply != nil){
			fmtprint(&f, "%s%s", round ? "\n" : "", reply);
			free(reply);
			ok = 1;
		}
		if(reply == nil || round >= autocont)
			break;
		/*
		 * Decide whether to auto-continue.  Two cases are
		 * recoverable; everything else stops the loop.
		 *
		 * 1. max_tokens: the round was guillotined mid-output.
		 *    claudeconverse already answered any orphaned
		 *    tool_use with a "not executed" result, so the
		 *    conversation is well-formed and "Continue." picks
		 *    up where the cut fell.  err is nil here.
		 *
		 * 2. tool loop limit: the per-prompt round cap (20) was
		 *    hit while the model was still calling tools.  This
		 *    sets err to the recoverable "tool loop limit
		 *    reached" string, but leaves the conversation
		 *    well-formed and ending on a tool_results user turn.
		 *    "Continue." (merged into that turn by buildreq)
		 *    lets the model resume.  Clear err so the
		 *    continuation round starts clean; if that round in
		 *    turn fails for real, its error is recorded then.
		 *
		 * A genuine API error (err set, not the tool-loop
		 * limit) must NOT be continued: resending a broken or
		 * wedged conversation just repeats the failure (this is
		 * how an earlier attempt turned one error into a loop of
		 * "request body is not valid JSON").
		 */
		if(err != nil){
			if(!toollimiterr(err))
				break;
			free(err);
			err = nil;
		}else if(u.stop_reason == nil
		|| strcmp(u.stop_reason, "max_tokens") != 0)
			break;
		qlock(&s->lk);
		convappend(s->conv, msgnew(Muser, "Continue.", nil));
		qunlock(&s->lk);
		free(u.stop_reason);
		u.stop_reason = nil;
	}

	streamfinish(s);
	reply = fmtstrflush(&f);

	qlock(&s->lk);
	s->busy = 0;
	bumpgraph();
	free(s->usage.stop_reason);
	s->usage = u;	/* struct copy; stop_reason ownership moves */
	if(!ok){
		char errbuf[512];
		if(err != nil)
			snprint(errbuf, sizeof errbuf, "%s", err);
		else
			rerrstr(errbuf, sizeof errbuf);
		/*
		 * An over-limit error wedges the session: the history
		 * is now too big for the model's context window, and
		 * every resend fails identically until it shrinks.
		 * Say so, and name the escape hatches, instead of
		 * leaving the user staring at a raw API string.
		 */
		if(overlimiterr(errbuf)){
			char *msg;
			msg = esmprint("%s\n"
				"context window exceeded; this session is wedged "
				"until history shrinks.  Drop old exchanges with "
				"'echo compact > ctl' (optionally 'compact N' to "
				"keep N recent exchanges) and resend, or 'echo clear "
				"> ctl' to start fresh.", errbuf);
			snprint(errbuf, sizeof errbuf, "%s", msg);
			free(msg);
		}
		free(err);
		free(reply);
		s->lasterror = estrdup(errbuf);
		free(s->lastreply);
		s->lastreply = nil;
		qunlock(&s->lk);
		respond(r, errbuf);
		return;
	}

	/*
	 * Partial success: we got some text, but a later round
	 * failed (API error mid-tool-loop, round cap, dropped
	 * connection).  Keep the partial reply readable, but
	 * record the error so it is visible in the error file
	 * instead of silently passing off a truncated answer
	 * as complete.
	 */
	if(err != nil)
		s->lasterror = err;	/* ownership moves */

	free(s->lastreply);
	s->lastreply = reply;
	qunlock(&s->lk);
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	uvlong path;
	int type, sid, hangup, reload, i;
	Session *s;
	char *data, *err;
	long count;

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	s = sessget(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}

	count = r->ifcall.count;
	data = emalloc(count + 1);
	memmove(data, r->ifcall.data, count);
	data[count] = '\0';
	while(count > 0 && (data[count-1] == '\n' || data[count-1] == '\r'))
		data[--count] = '\0';

	switch(type){
	case Qprompt:
		if(count == 0){
			free(data);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
			break;
		}
		srvrelease(&clsrv);
		doprompt(r, s, data);
		srvacquire(&clsrv);
		break;

	case Qctl:
		hangup = 0;
		reload = 0;
		qlock(&s->lk);
		err = handlectl(s, data, &hangup, &reload);
		qunlock(&s->lk);
		free(data);
		if(err != nil){
			respond(r, err);
			break;
		}
		if(hangup)
			delsession(sid);
		if(reload)
			doreloadskills();
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		break;

	default:
		/* settings files: dispatch through the table */
		for(i = 0; i < nelem(sessfiles); i++)
			if(sessfiles[i].type == type && sessfiles[i].wr != nil)
				break;
		if(i == nelem(sessfiles)){
			free(data);
			respond(r, "permission denied");
			break;
		}
		qlock(&s->lk);
		if(s->busy){
			qunlock(&s->lk);
			free(data);
			respond(r, "session busy");
			break;
		}
		err = sessfiles[i].wr(s, data);
		qunlock(&s->lk);
		free(data);
		if(err != nil){
			respond(r, err);
			break;
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		break;
	}
	sessput(s);
}

static void
fsdestroyfid(Fid *fid)
{
	Faux *fa;

	fa = fid->aux;
	if(fa != nil){
		/* drop the reference held by a clone fid */
		sessput(fa->clone);
		free(fa->graphbuf);
		free(fa);
		fid->aux = nil;
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-K skillsdir] [-n namepath] [-s srvname] [-m mtpt] [-M model] [-t maxtokens]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *mtpt;

	srvname = nil;
	mtpt = "/mnt/claude";

	ARGBEGIN{
	case 'n':
		namepath = EARGF(usage());
		break;
	case 'K':
		skillsdir = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'M':
		defmodel = EARGF(usage());
		break;
	case 't':
		defmaxtokens = atoi(EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND

	if(argc != 0)
		usage();

	apikey = getenv("ANTHROPIC_API_KEY");
	if(apikey == nil || apikey[0] == '\0'){
		fprint(2, "set $ANTHROPIC_API_KEY\n");
		threadexitsall("no api key");
	}

	if(skillsdir != nil){
		defskills = readskills(skillsdir);
		if(defskills != nil)
			fprint(2, "loaded skills from %s\n", skillsdir);
	}

	graphrz.l = &graphlk;

	memset(&clsrv, 0, sizeof clsrv);
	clsrv.attach = fsattach;
	clsrv.walk1 = fswalk1;
	clsrv.open = fsopen;
	clsrv.stat = fsstat;
	clsrv.read = fsread;
	clsrv.write = fswrite;
	clsrv.flush = fsflush;
	clsrv.destroyfid = fsdestroyfid;
	threadpostmountsrv(&clsrv, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
