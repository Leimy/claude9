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
static char *defmodel = "claude-sonnet-4-20250514";
static int defmaxtokens = 16384;
static char *defsysprompt = nil;

static Session *sessions;
static int nextsid;
static QLock sessionlk;

static Session*
newsession(void)
{
	Session *s;

	s = mallocz(sizeof *s, 1);
	if(s == nil)
		sysfatal("malloc: %r");
	qlock(&sessionlk);
	s->id = nextsid++;
	s->conv = convnew(apikey, defmodel, defmaxtokens, defsysprompt);
	s->streamrz.l = &s->streamlk;
	s->streamdone = 1;
	s->next = sessions;
	sessions = s;
	qunlock(&sessionlk);
	return s;
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

static void
freesession(Session *s)
{
	if(s == nil)
		return;
	convfree(s->conv);
	free(s->lastreply);
	free(s->lasterror);
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
 * Streaming helpers.  Text deltas from claudeconverse_stream
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
		s->streambuf = realloc(s->streambuf, s->streamcap);
		if(s->streambuf == nil)
			sysfatal("realloc: %r");
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
 * Callback invoked by claudeconverse_stream with each incremental
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

static char *msgsep = "---msg";
static char *modelhdr = "---model";
static char *maxtokenshdr = "---maxtokens";
static char *sysprompthdr = "---sysprompt";

static void
sessionsave(Session *s, char *path)
{
	Msg *m;
	int fd;
	char *sol, *eol;

	mkparents(path);
	fd = create(path, OWRITE, 0666);
	if(fd < 0)
		return;

	fprint(fd, "%s %s\n", modelhdr, s->conv->model);
	fprint(fd, "%s %d\n", maxtokenshdr, s->conv->maxtokens);
	if(s->conv->sysprompt != nil){
		fprint(fd, "%s\n", sysprompthdr);
		fprint(fd, "%s\n", s->conv->sysprompt);
	}

	for(m = s->conv->msgs; m != nil; m = m->next){
		fprint(fd, "%s %s\n", msgsep,
			m->role == Muser ? "user" : "assistant");
		for(sol = m->text; *sol != '\0'; sol = eol){
			eol = strchr(sol, '\n');
			if(eol != nil)
				eol++;
			else
				eol = sol + strlen(sol);
			if(strncmp(sol, "---", 3) == 0)
				fprint(fd, " %.*s", (int)(eol - sol), sol);
			else
				write(fd, sol, eol - sol);
		}
		if(m->text[0] != '\0' && m->text[strlen(m->text)-1] != '\n')
			fprint(fd, "\n");
	}
	close(fd);
}

static void
sessionload(Session *s, char *path)
{
	int fd;
	char *data, *p, *line, *eol, *v, *r;
	Msg *m, *next;
	int msglen, msgcap, llen, need, role;
	char *msgbuf;
	enum { Shdr, Ssysprompt, Smsg } state;

	fd = open(path, OREAD);
	if(fd < 0)
		return;
	data = readfile(fd);
	close(fd);
	if(data == nil)
		return;

	for(m = s->conv->msgs; m != nil; m = next){
		next = m->next;
		free(m->text);
		free(m->rawjson);
		free(m);
	}
	s->conv->msgs = nil;
	s->conv->tail = nil;

	state = Shdr;
	role = Muser;
	msgbuf = nil;
	msglen = 0;
	msgcap = 0;

	for(p = data; *p != '\0'; ){
		line = p;
		eol = strchr(p, '\n');
		if(eol != nil){
			*eol = '\0';
			p = eol + 1;
		} else
			p += strlen(p);

		if(strncmp(line, msgsep, strlen(msgsep)) == 0){
			if(state == Smsg && msgbuf != nil){
				while(msglen > 0 && msgbuf[msglen-1] == '\n')
					msglen--;
				msgbuf[msglen] = '\0';
				convappend(s->conv, msgnew(role, msgbuf));
			}
			if(state == Ssysprompt && msgbuf != nil){
				while(msglen > 0 && msgbuf[msglen-1] == '\n')
					msglen--;
				msgbuf[msglen] = '\0';
				free(s->conv->sysprompt);
				s->conv->sysprompt = strdup(msgbuf);
			}
			r = line + strlen(msgsep);
			while(*r == ' ') r++;
			role = (strcmp(r, "assistant") == 0) ? Massistant : Muser;
			state = Smsg;
			msglen = 0;
			continue;
		}

		if(state == Shdr){
			if(strncmp(line, modelhdr, strlen(modelhdr)) == 0){
				v = line + strlen(modelhdr);
				while(*v == ' ') v++;
				if(*v != '\0'){
					free(s->conv->model);
					s->conv->model = strdup(v);
				}
			} else if(strncmp(line, maxtokenshdr, strlen(maxtokenshdr)) == 0){
				v = line + strlen(maxtokenshdr);
				while(*v == ' ') v++;
				if(*v != '\0')
					s->conv->maxtokens = atoi(v);
			} else if(strncmp(line, sysprompthdr, strlen(sysprompthdr)) == 0){
				state = Ssysprompt;
				msglen = 0;
				continue;
			}
			continue;
		}

		if(state == Ssysprompt || state == Smsg){
			if(line[0] == ' ' && strncmp(line+1, "---", 3) == 0)
				line++;
			llen = strlen(line);
			need = msglen + llen + 2;
			if(need > msgcap){
				while(need > msgcap)
					msgcap = msgcap ? msgcap * 2 : 1024;
				msgbuf = realloc(msgbuf, msgcap);
				if(msgbuf == nil)
					sysfatal("realloc: %r");
			}
			if(msglen > 0)
				msgbuf[msglen++] = '\n';
			memmove(msgbuf + msglen, line, llen);
			msglen += llen;
			msgbuf[msglen] = '\0';
		}
	}

	if(state == Smsg && msgbuf != nil && msglen > 0){
		while(msglen > 0 && msgbuf[msglen-1] == '\n')
			msglen--;
		msgbuf[msglen] = '\0';
		convappend(s->conv, msgnew(role, msgbuf));
	}
	if(state == Ssysprompt && msgbuf != nil && msglen > 0){
		while(msglen > 0 && msgbuf[msglen-1] == '\n')
			msglen--;
		msgbuf[msglen] = '\0';
		free(s->conv->sysprompt);
		s->conv->sysprompt = strdup(msgbuf);
	}
	free(msgbuf);
	free(data);
}

static int
rootgen(int i, Dir *d, void *v)
{
	Session *s;
	int n;
	char buf[32];

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
			snprint(buf, sizeof buf, "%d", s->id);
			d->qid = (Qid){QPATH(s->id, Qsess), 0, QTDIR};
			d->mode = DMDIR|0555;
			d->atime = d->mtime = time(0);
			d->length = 0;
			d->name = estrdup9p(buf);
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

static char *sessfiles[] = {
	"ctl", "prompt", "conv", "model", "tokens", "system", "usage", "error", "stream",
};
static int sesstypes[] = {
	Qctl, Qprompt, Qconv, Qmodel, Qtokens, Qsystem, Qusage, Qerror, Qstream,
};

static int
sessgen(int i, Dir *d, void *aux)
{
	Session *s;
	int sid, mode;

	sid = (int)(uintptr)aux;
	s = findsession(sid);
	if(s == nil)
		return -1;
	if(i < 0 || i >= nelem(sessfiles))
		return -1;

	switch(sesstypes[i]){
	case Qctl:
	case Qprompt:
	case Qmodel:
	case Qtokens:
	case Qsystem:
	case Qconv:
		mode = 0666;
		break;
	default:
		mode = 0444;
		break;
	}

	d->qid = (Qid){QPATH(sid, sesstypes[i]), 0, QTFILE};
	d->mode = mode;
	d->atime = d->mtime = time(0);
	d->length = 0;
	d->name = estrdup9p(sessfiles[i]);
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
	buf = malloc(sz);
	if(buf == nil)
		return estrdup9p("");
	n = 0;
	for(m = c->msgs; m != nil; m = m->next){
		role = m->role == Muser ? "user" : "assistant";
		while(n + (int)strlen(role) + (int)strlen(m->text) + 8 >= sz){
			sz *= 2;
			buf = realloc(buf, sz);
			if(buf == nil)
				return estrdup9p("");
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
	char buf[256];

	snprint(buf, sizeof buf,
		"input_tokens %d\n"
		"output_tokens %d\n"
		"total_tokens %d\n"
		"stop_reason %s\n",
		s->usage.input_tokens,
		s->usage.output_tokens,
		s->usage.input_tokens + s->usage.output_tokens,
		s->usage.stop_reason ? s->usage.stop_reason : "none");
	return estrdup9p(buf);
}

static char*
ctltext(Session *s)
{
	char buf[512];

	snprint(buf, sizeof buf,
		"model %s\n"
		"tokens %d\n"
		"messages %d\n"
		"bytes %ld\n",
		s->conv->model,
		s->conv->maxtokens,
		convcount(s->conv),
		convsize(s->conv));
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
	int type, sid, i;
	Session *s;
	char buf[32];

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
		sid = atoi(name);
		snprint(buf, sizeof buf, "%d", sid);
		if(strcmp(buf, name) != 0)
			return "not found";
		s = findsession(sid);
		if(s == nil)
			return "not found";
		*qid = (Qid){QPATH(sid, Qsess), 0, QTDIR};
		return nil;
	}
	if(type == Qsess){
		sid = QSID(path);
		s = findsession(sid);
		if(s == nil)
			return "session gone";
		for(i = 0; i < nelem(sessfiles); i++){
			if(strcmp(name, sessfiles[i]) == 0){
				*qid = (Qid){QPATH(sid, sesstypes[i]), 0, QTFILE};
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
	int type, sid;
	Session *s;
	char buf[32];

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
		snprint(buf, sizeof buf, "%d", sid);
		r->d.name = estrdup9p(buf);
		r->d.qid = r->fid->qid;
		r->d.mode = DMDIR|0555;
		respond(r, nil);
		return;
	}

	s = findsession(sid);
	if(s == nil){
		respond(r, "session gone");
		return;
	}
	r->d.qid = r->fid->qid;
	switch(type){
	case Qctl:
		r->d.name = estrdup9p("ctl");
		r->d.mode = 0666;
		break;
	case Qprompt:
		r->d.name = estrdup9p("prompt");
		r->d.mode = 0666;
		break;
	case Qconv:
		r->d.name = estrdup9p("conv");
		r->d.mode = 0666;
		break;
	case Qmodel:
		r->d.name = estrdup9p("model");
		r->d.mode = 0666;
		break;
	case Qtokens:
		r->d.name = estrdup9p("tokens");
		r->d.mode = 0666;
		break;
	case Qsystem:
		r->d.name = estrdup9p("system");
		r->d.mode = 0666;
		break;
	case Qusage:
		r->d.name = estrdup9p("usage");
		r->d.mode = 0444;
		break;
	case Qerror:
		r->d.name = estrdup9p("error");
		r->d.mode = 0444;
		break;
	case Qstream:
		r->d.name = estrdup9p("stream");
		r->d.mode = 0444;
		break;
	default:
		respond(r, "unknown file");
		return;
	}
	respond(r, nil);
}

static void
modelstext(Req *r)
{
	ModelInfo *list;
	int n, i;
	char *buf, *tmp;
	int len, cap, slen;
	char line[256];

	n = fetchmodels(apikey, &list);
	if(n < 0){
		readstr(r, "error fetching models\n");
		respond(r, nil);
		return;
	}
	cap = 4096;
	buf = malloc(cap);
	if(buf == nil)
		sysfatal("malloc: %r");
	len = 0;
	for(i = 0; i < n; i++){
		if(list[i].id == nil)
			continue;
		snprint(line, sizeof line, "%s\n", list[i].id);
		slen = strlen(line);
		while(len + slen + 1 > cap){
			cap *= 2;
			tmp = realloc(buf, cap);
			if(tmp == nil)
				sysfatal("realloc: %r");
			buf = tmp;
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
 * fact, read the prompt file instead.  This matches the design
 * in STREAMING.md and is what claudetalk relies on: it opens the
 * stream file and backgrounds the reader before writing to
 * prompt, expecting the reader to receive the new round's text.
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
	 * next round to start -- regardless of whether there is
	 * still buffered text from the previous round.  That
	 * buffered text belongs to the PREVIOUS reader, not us;
	 * replaying it would make claudetalk print the last reply
	 * again when the user sends a new message.
	 *
	 * If the session was already mid-round at open time, pin
	 * to that live generation so we stream its output.
	 */
	if(!fa->streamgenset){
		if(fa->streamopendone){
			/*
			 * Opened against an idle session: wait for
			 * a new round (streamgen must advance AND
			 * streamdone must have flipped to 0).  The
			 * streamreset() call in fswrite does both
			 * atomically under streamlk, so one wakeup
			 * is enough.
			 */
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
			data = malloc(want);
			if(data == nil){
				qunlock(&s->streamlk);
				respond(r, "out of memory");
				return;
			}
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
			/*
			 * Our generation is done and we've read all its
			 * bytes.  EOF.
			 */
			qunlock(&s->streamlk);
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		rsleep(&s->streamrz);
	}
}

/*
 * On open, allocate Faux and snapshot session state so that
 * streamread can tell whether this fid opened against an idle
 * session (in which case it should wait for the next round) or
 * mid-round (in which case it should stream the current round).
 * This eliminates the race window between "cat stream &" and
 * "echo > prompt" in claudetalk: whichever 9p op lands first
 * here will see a consistent snapshot under streamlk.
 */
static void
fsopen(Req *r)
{
	Faux *fa;
	uvlong path;
	int type, sid;
	Session *s;

	fa = r->fid->aux;
	if(fa == nil){
		fa = mallocz(sizeof *fa, 1);
		if(fa == nil)
			sysfatal("malloc: %r");
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
		fa = mallocz(sizeof *fa, 1);
		if(fa == nil)
			sysfatal("malloc: %r");
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
		snprint(buf, sizeof buf, "%d", s->id);
		readstr(r, buf);
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
	} else if(strncmp(cmd, "save ", 5) == 0){
		char *path;
		path = cmd + 5;
		while(*path == ' ') path++;
		if(*path != '\0')
			sessionsave(s, path);
	} else if(strncmp(cmd, "load ", 5) == 0){
		char *path;
		path = cmd + 5;
		while(*path == ' ') path++;
		if(*path != '\0')
			sessionload(s, path);
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
	data = malloc(count + 1);
	if(data == nil){
		respond(r, "out of memory");
		return;
	}
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

		/*
		 * Reset streaming buffer *before* we kick off the work
		 * so any cat of$sess/stream started in parallel with
		 * us will block on new data rather than see stale text
		 * or an immediate EOF.
		 */
		streamreset(s);

		reply = claudeconverse_stream(s->conv, &s->usage,
			streamcb, s);

		streamfinish(s);

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

		/* claudeconverse already appends all messages to conv */
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
	fprint(2, "usage: %s [-s srvname] [-m mtpt] [-M model] [-t maxtokens]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *mtpt;

	srvname = nil;
	mtpt = "/mnt/claude";

	ARGBEGIN{
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
		fprint(2, "set$ANTHROPIC_API_KEY\n");
		exits("no api key");
	}

	initclsrv();
	threadpostmountsrv(&clsrv, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
