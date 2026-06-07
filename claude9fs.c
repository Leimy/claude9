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
};

#define QPATH(sid, type)	((uvlong)(sid)<<8 | (type))
#define QSID(path)		((int)((path)>>8))
#define QTYPE(path)		((int)((path)&0xff))

typedef struct Session Session;
struct Session {
	int id;
	char *name;
	Conv *conv;
	char *lastreply;
	char *lasterror;
	Usage usage;
	QLock lk;
	/* streaming */
	QLock streamlk;
	Rendez streamrz;
	char *streambuf;
	int streamlen;
	int streamcap;
	int streamdone;
	int streamgen;
	/* auto-continue on max_tokens */
	int autocont;	/* max auto-continue rounds; 0 = disabled */
	Session *next;
};

typedef struct Faux Faux;
struct Faux {
	Session *clone;
	int streamoff;
	int streamgen;
	int streamgenset;
	int streamopengen;	/* s->streamgen captured at open time */
	int streamopendone;	/* s->streamdone captured at open time */
};

static Srv clsrv;

static char *apikey;
static char *defmodel = "claude-opus-4-6";
static int defmaxtokens = 16384;
static char *defsysprompt = nil;
static char *skillsdir = nil;
static char *defskills = nil;
static char *namepath = "/mnt/names/name";

static Session *sessions;
static int nextsid;
static QLock sessionlk;

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
	int dfd, fd, n, i;
	int gotany;
	Dir *d;
	char *path, *data, *buf;
	int len, cap, dlen, nlen, hlen;
	char header[512];

	if(dir == nil || dir[0] == '\0')
		return nil;

	dfd = open(dir, OREAD);
	if(dfd < 0)
		return nil;

	cap = 4096;
	buf = emalloc(cap);
	len = 0;

	/* section header */
	hlen = snprint(header, sizeof header,
		"\n\nSkills\n"
		"------\n"
		"The following skill files were loaded at startup from %s.\n"
		"Follow their instructions.\n\n", dir);

	while(len + hlen + 1 > cap){
		cap *= 2;
		buf = erealloc(buf, cap);
	}
	memmove(buf + len, header, hlen);
	len += hlen;
	buf[len] = '\0';

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
			nlen = strlen(d[i].name);

			/* "### filename\n" + data + "\n\n" */
			while(len + nlen + dlen + 8 > cap){
				cap *= 2;
				buf = erealloc(buf, cap);
			}
			len += snprint(buf + len, cap - len, "### %s\n", d[i].name);
			memmove(buf + len, data, dlen);
			len += dlen;
			if(dlen > 0 && data[dlen-1] != '\n')
				buf[len++] = '\n';
			buf[len++] = '\n';
			buf[len] = '\0';
			free(data);
			gotany = 1;
		}
		free(d);
	}
	close(dfd);

	if(!gotany){
		free(buf);
		return nil;
	}
	return buf;
}

static Session*
findsession(int id)
{
	Session *s;

	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next)
		if(s->id == id)
			break;
	qunlock(&sessionlk);
	return s;
}

static Session*
findsessionname(char *name)
{
	Session *s;

	qlock(&sessionlk);
	for(s = sessions; s != nil; s = s->next)
		if(strcmp(s->name, name) == 0)
			break;
	qunlock(&sessionlk);
	return s;
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

static Session*
newsession(void)
{
	Session *s;
	char *name;
	char buf[32];

	s = emallocz(sizeof *s, 1);
	qlock(&sessionlk);
	s->id = nextsid++;
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
	s->conv = convnew(apikey, defmodel, defmaxtokens, defsysprompt, defskills);
	s->streamrz.l = &s->streamlk;
	s->streamdone = 1;
	s->next = sessions;
	sessions = s;
	qunlock(&sessionlk);
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
	free(s->usage.stop_reason);
	free(s->streambuf);
	free(s);
}

static void
delsession(int id)
{
	Session **pp, *s;

	qlock(&sessionlk);
	for(pp = &sessions; *pp != nil; pp = &(*pp)->next){
		if((*pp)->id == id){
			s = *pp;
			*pp = s->next;
			qunlock(&sessionlk);
			freesession(s);
			return;
		}
	}
	qunlock(&sessionlk);
}

/*
 * Streaming helpers.  Text deltas from claudeconverse
 * are appended to s->streambuf; blocked readers on Qstream are
 * woken via s->streamrz.  All state guarded by s->streamlk.
 */
static void
streamreset(Session *s)
{
	qlock(&s->streamlk);
	/* Completely clear the buffer */
	s->streamlen = 0;
	if(s->streambuf != nil){
		s->streambuf[0] = '\0';
		memset(s->streambuf, 0, s->streamcap);
	}
	s->streamdone = 0;
	s->streamgen++;
	rwakeupall(&s->streamrz);
	qunlock(&s->streamlk);
}

static void
streamappend(Session *s, char *data, int n)
{
	int need;

	qlock(&s->streamlk);
	need = s->streamlen + n + 1;
	if(need > s->streamcap){
		while(need > s->streamcap)
			s->streamcap = s->streamcap ? s->streamcap * 2 : 4096;
		s->streambuf = erealloc(s->streambuf, s->streamcap);
	}
	memmove(s->streambuf + s->streamlen, data, n);
	s->streamlen += n;
	s->streambuf[s->streamlen] = '\0';
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
 * Session file table: maps file names to qid types and modes.
 */
static struct {
	char *name;
	int type;
	int mode;
} sessfiles[] = {
	{ "ctl",	Qctl,		0666 },
	{ "prompt",	Qprompt,	0666 },
	{ "conv",	Qconv,		0666 },
	{ "model",	Qmodel,		0666 },
	{ "tokens",	Qtokens,	0666 },
	{ "system",	Qsystem,	0666 },
	{ "usage",	Qusage,		0444 },
	{ "error",	Qerror,		0444 },
	{ "stream",	Qstream,	0444 },
};

static int
rootgen(int i, Dir *d, void *v)
{
	Session *s;
	int n;

	USED(v);
	if(i == 0){
		d->qid = (Qid){Qclone, 0, QTFILE};
		d->mode = 0444;
		d->atime = d->mtime = time(0);
		d->length = 0;
		d->name = estrdup9p("clone");
		d->uid = estrdup9p("claude");
		d->gid = estrdup9p("claude");
		d->muid = estrdup9p("");
		return 0;
	}
	i--;
	if(i == 0){
		d->qid = (Qid){Qmodels, 0, QTFILE};
		d->mode = 0444;
		d->atime = d->mtime = time(0);
		d->length = 0;
		d->name = estrdup9p("models");
		d->uid = estrdup9p("claude");
		d->gid = estrdup9p("claude");
		d->muid = estrdup9p("");
		return 0;
	}
	i--;
	qlock(&sessionlk);
	n = 0;
	for(s = sessions; s != nil; s = s->next){
		if(n == i){
			d->qid = (Qid){QPATH(s->id, Qsess), 0, QTDIR};
			d->mode = DMDIR|0555;
			d->atime = d->mtime = time(0);
			d->length = 0;
			d->name = estrdup9p(s->name);
			d->uid = estrdup9p("claude");
			d->gid = estrdup9p("claude");
			d->muid = estrdup9p("");
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
	Session *s;
	int sid;

	sid = (int)(uintptr)aux;
	s = findsession(sid);
	if(s == nil)
		return -1;
	if(i < 0 || i >= nelem(sessfiles))
		return -1;

	d->qid = (Qid){QPATH(sid, sessfiles[i].type), 0, QTFILE};
	d->mode = sessfiles[i].mode;
	d->atime = d->mtime = time(0);
	d->length = 0;
	d->name = estrdup9p(sessfiles[i].name);
	d->uid = estrdup9p("claude");
	d->gid = estrdup9p("claude");
	d->muid = estrdup9p("");
	return 0;
}

static char*
convtext(Conv *c)
{
	Msg *m;
	int sz, n;
	char *buf, *role;

	sz = 1024;
	buf = emalloc(sz);
	n = 0;
	for(m = c->msgs; m != nil; m = m->next){
		role = m->role == Muser ? "user" : "assistant";
		while(n + (int)strlen(role) + (int)strlen(m->text) + 8 >= sz){
			sz *= 2;
			buf = erealloc(buf, sz);
		}
		n += snprint(buf + n, sz - n, "[%s]\n%s\n\n", role, m->text);
	}
	if(n == 0)
		buf[0] = '\0';
	return buf;
}

static char*
usagetext(Session *s)
{
	char buf[512];

	snprint(buf, sizeof buf,
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
	return estrdup9p(buf);
}

static char*
ctltext(Session *s)
{
	char buf[512];

	snprint(buf, sizeof buf,
		"name %s\n"
		"model %s\n"
		"tokens %d\n"
		"messages %d\n"
		"bytes %ld\n"
		"autocontinue %d\n",
		s->name,
		s->conv->model,
		s->conv->maxtokens,
		convcount(s->conv),
		convsize(s->conv),
		s->autocont);
	return estrdup9p(buf);
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
		if(strcmp(name, "clone") == 0){
			*qid = (Qid){Qclone, 0, QTFILE};
			return nil;
		}
		if(strcmp(name, "models") == 0){
			*qid = (Qid){Qmodels, 0, QTFILE};
			return nil;
		}
		s = findsessionname(name);
		if(s == nil)
			return "not found";
		*qid = (Qid){QPATH(s->id, Qsess), 0, QTDIR};
		return nil;
	}
	if(type == Qsess){
		int sid;
		sid = QSID(path);
		s = findsession(sid);
		if(s == nil)
			return "session gone";
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

	r->d.uid = estrdup9p("claude");
	r->d.gid = estrdup9p("claude");
	r->d.muid = estrdup9p("");
	r->d.atime = r->d.mtime = time(0);
	r->d.length = 0;

	if(path == Qroot){
		r->d.name = estrdup9p("/");
		r->d.qid = (Qid){Qroot, 0, QTDIR};
		r->d.mode = DMDIR|0555;
		respond(r, nil);
		return;
	}
	if(path == Qclone){
		r->d.name = estrdup9p("clone");
		r->d.qid = (Qid){Qclone, 0, QTFILE};
		r->d.mode = 0444;
		respond(r, nil);
		return;
	}
	if(path == Qmodels){
		r->d.name = estrdup9p("models");
		r->d.qid = (Qid){Qmodels, 0, QTFILE};
		r->d.mode = 0444;
		respond(r, nil);
		return;
	}
	if(type == Qsess){
		s = findsession(sid);
		if(s == nil){
			respond(r, "session gone");
			return;
		}
		r->d.name = estrdup9p(s->name);
		r->d.qid = r->fid->qid;
		r->d.mode = DMDIR|0555;
		respond(r, nil);
		return;
	}

	/* session file -- look up in table */
	s = findsession(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}
	r->d.qid = r->fid->qid;
	for(i = 0; i < nelem(sessfiles); i++){
		if(sessfiles[i].type == type){
			r->d.name = estrdup9p(sessfiles[i].name);
			r->d.mode = sessfiles[i].mode;
			respond(r, nil);
			return;
		}
	}
	respond(r, "unknown file");
}

static void
modelstext(Req *r)
{
	ModelInfo *list;
	int n, i;
	char *buf;
	int len, cap, slen;
	char line[256];

	n = fetchmodels(apikey, &list);
	if(n < 0){
		readstr(r, "error fetching models\n");
		respond(r, nil);
		return;
	}
	cap = 4096;
	buf = emalloc(cap);
	len = 0;
	for(i = 0; i < n; i++){
		if(list[i].id == nil)
			continue;
		snprint(line, sizeof line, "%s\n", list[i].id);
		slen = strlen(line);
		while(len + slen + 1 > cap){
			cap *= 2;
			buf = erealloc(buf, cap);
		}
		memmove(buf + len, line, slen);
		len += slen;
	}
	buf[len] = '\0';
	for(i = 0; i < n; i++){
		free(list[i].id);
		free(list[i].display_name);
	}
	free(list);
	readstr(r, buf);
	free(buf);
	respond(r, nil);
}

/*
 * Blocking read on Qstream.  Returns new bytes past fa->streamoff,
 * or EOF when the current round's stream is done, or blocks
 * waiting for data / end-of-round / start-of-next-round.
 *
 * A newly-opened fid whose session was idle at open time (as
 * captured by fsopen into fa->streamopendone) blocks until the
 * next round starts.  To see the last round's text after the
 * fact, read the prompt file instead.
 */
static void
streamread(Req *r, Session *s, Faux *fa)
{
	long avail, want, off;
	char *data;

	qlock(&s->streamlk);
	/*
	 * On first read, decide which generation this fid belongs
	 * to.  If the session was idle at open time, wait for the
	 * next round to start.
	 */
	if(!fa->streamgenset){
		if(fa->streamopendone){
			while(s->streamgen == fa->streamopengen
			   && s->streamdone)
				rsleep(&s->streamrz);
		}
		fa->streamgen = s->streamgen;
		fa->streamgenset = 1;
	}
	for(;;){
		/* fid's generation retired -- EOF */
		if(fa->streamgen != s->streamgen){
			qunlock(&s->streamlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if(fa->streamoff < s->streamlen){
			avail = s->streamlen - fa->streamoff;
			want = r->ifcall.count;
			if(want > avail) want = avail;
			data = emalloc(want);
			memmove(data, s->streambuf + fa->streamoff, want);
			off = fa->streamoff;
			fa->streamoff += want;
			qunlock(&s->streamlk);
			memmove(r->ofcall.data, data, want);
			r->ofcall.count = want;
			free(data);
			USED(off);
			respond(r, nil);
			return;
		}
		if(s->streamdone){
			qunlock(&s->streamlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		rsleep(&s->streamrz);
	}
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

	if(type == Qstream){
		s = findsession(sid);
		if(s != nil){
			qlock(&s->streamlk);
			fa->streamopengen = s->streamgen;
			fa->streamopendone = s->streamdone;
			qunlock(&s->streamlk);
		}
	}
	respond(r, nil);
}

static void
fsread(Req *r)
{
	uvlong path;
	int type, sid;
	Session *s;
	char buf[64];
	char *text;
	Faux *fa;

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	fa = r->fid->aux;
	if(fa == nil){
		fa = emallocz(sizeof *fa, 1);
		r->fid->aux = fa;
	}

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

	if(type == Qsess){
		dirread9p(r, sessgen, (void*)(uintptr)sid);
		respond(r, nil);
		return;
	}

	s = findsession(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}

	switch(type){
	case Qctl:
		text = ctltext(s);
		readstr(r, text);
		free(text);
		respond(r, nil);
		return;
	case Qprompt:
		qlock(&s->lk);
		readstr(r, s->lastreply ? s->lastreply : "");
		qunlock(&s->lk);
		respond(r, nil);
		return;
	case Qconv:
		qlock(&s->lk);
		text = convtext(s->conv);
		qunlock(&s->lk);
		readstr(r, text);
		free(text);
		respond(r, nil);
		return;
	case Qmodel:
		qlock(&s->lk);
		readstr(r, s->conv->model);
		qunlock(&s->lk);
		respond(r, nil);
		return;
	case Qtokens:
		snprint(buf, sizeof buf, "%d", s->conv->maxtokens);
		readstr(r, buf);
		respond(r, nil);
		return;
	case Qsystem:
		qlock(&s->lk);
		readstr(r, s->conv->sysprompt ? s->conv->sysprompt : "");
		qunlock(&s->lk);
		respond(r, nil);
		return;
	case Qusage:
		text = usagetext(s);
		readstr(r, text);
		free(text);
		respond(r, nil);
		return;
	case Qerror:
		qlock(&s->lk);
		readstr(r, s->lasterror ? s->lasterror : "");
		qunlock(&s->lk);
		respond(r, nil);
		return;
	case Qstream:
		srvrelease(&clsrv);
		streamread(r, s, fa);
		srvacquire(&clsrv);
		return;
	}
	respond(r, "bug in fsread");
}

static void
handlectl(Session *s, char *cmd, int *hangup)
{
	Msg *m, *next;

	if(strcmp(cmd, "clear") == 0){
		for(m = s->conv->msgs; m != nil; m = next){
			next = m->next;
			free(m->text);
			free(m->rawjson);
			free(m);
		}
		s->conv->msgs = nil;
		s->conv->tail = nil;
		free(s->lastreply);
		s->lastreply = nil;
		free(s->lasterror);
		s->lasterror = nil;
		free(s->usage.stop_reason);
		memset(&s->usage, 0, sizeof s->usage);
		qlock(&s->streamlk);
		s->streamlen = 0;
		if(s->streambuf != nil)
			s->streambuf[0] = '\0';
		s->streamdone = 1;
		s->streamgen++;
		rwakeupall(&s->streamrz);
		qunlock(&s->streamlk);
	} else if(strcmp(cmd, "hangup") == 0){
		*hangup = 1;
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
	}
}

static void
fswrite(Req *r)
{
	uvlong path;
	int type, sid;
	int hangup, n;
	Session *s;
	char *data, *reply;
	char *contreply;
	long count;

	path = r->fid->qid.path;
	type = QTYPE(path);
	sid = QSID(path);

	s = findsession(sid);
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
			return;
		}
		srvrelease(&clsrv);

		qlock(&s->lk);
		free(s->lasterror);
		s->lasterror = nil;
		free(s->usage.stop_reason);
		memset(&s->usage, 0, sizeof s->usage);
		convappend(s->conv, msgnew(Muser, data));
		free(data);
		qunlock(&s->lk);

		streamreset(s);

		reply = claudeconverse(s->conv, &s->usage,
			streamcb, s);

		streamfinish(s);

		/*
		 * Auto-continue: if Claude hit max_tokens and
		 * autocontinue is enabled, send "Continue." messages
		 * to keep going.
		 */
		if(reply != nil && s->autocont > 0){
			int contround;
			for(contround = 0; contround < s->autocont; contround++){
				if(s->usage.stop_reason == nil
				|| strcmp(s->usage.stop_reason, "max_tokens") != 0)
					break;

				qlock(&s->lk);
				convappend(s->conv, msgnew(Muser, "Continue."));
				free(s->usage.stop_reason);
				s->usage.stop_reason = nil;
				qunlock(&s->lk);

				qlock(&s->streamlk);
				s->streamdone = 0;
				qunlock(&s->streamlk);

				contreply = claudeconverse(s->conv,
					&s->usage, streamcb, s);

				streamfinish(s);

				if(contreply != nil){
					reply = erealloc(reply,
						strlen(reply) + strlen(contreply) + 2);
					strcat(reply, "\n");
					strcat(reply, contreply);
					free(contreply);
				} else
					break;
			}
		}

		qlock(&s->lk);
		if(reply == nil){
			char errbuf[256];
			rerrstr(errbuf, sizeof errbuf);
			s->lasterror = estrdup9p(errbuf);
			free(s->lastreply);
			s->lastreply = nil;
			qunlock(&s->lk);
			srvacquire(&clsrv);
			respond(r, errbuf);
			return;
		}

		free(s->lastreply);
		s->lastreply = estrdup9p(reply);
		free(reply);
		qunlock(&s->lk);
		srvacquire(&clsrv);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Qctl:
		hangup = 0;
		qlock(&s->lk);
		handlectl(s, data, &hangup);
		qunlock(&s->lk);
		free(data);
		if(hangup)
			delsession(sid);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Qmodel:
		qlock(&s->lk);
		free(s->conv->model);
		s->conv->model = data;
		qunlock(&s->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Qtokens:
		n = atoi(data);
		free(data);
		if(n <= 0){
			respond(r, "invalid token count");
			return;
		}
		qlock(&s->lk);
		s->conv->maxtokens = n;
		qunlock(&s->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Qsystem:
		qlock(&s->lk);
		free(s->conv->sysprompt);
		s->conv->sysprompt = data;
		qunlock(&s->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	default:
		free(data);
		respond(r, "permission denied");
		return;
	}
}

static void
fsdestroyfid(Fid *fid)
{
	Faux *fa;

	fa = fid->aux;
	if(fa != nil){
		free(fa);
		fid->aux = nil;
	}
}

void
initclsrv(void)
{
	memset(&clsrv, 0, sizeof clsrv);
	clsrv.attach = fsattach;
	clsrv.walk1 = fswalk1;
	clsrv.open = fsopen;
	clsrv.stat = fsstat;
	clsrv.read = fsread;
	clsrv.write = fswrite;
	clsrv.destroyfid = fsdestroyfid;
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

	initclsrv();
	threadpostmountsrv(&clsrv, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
