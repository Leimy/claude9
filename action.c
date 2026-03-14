#include <u.h>
#include <libc.h>
#include "json.h"
#include "claude.h"

static char *actionstart = "<<<ACTION";
static char *actionend = ">>>ACTION";

static int
parsehdr(Action *a, char *hdr)
{
	char *p, *tok;

	a->type = -1;
	a->path = nil;

	p = hdr;
	while(*p != '\0'){
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p == '\0')
			break;
		tok = p;
		while(*p != '\0' && *p != ' ' && *p != '\t')
			p++;
		if(*p != '\0')
			*p++ = '\0';

		if(strncmp(tok, "file:", 5) == 0){
			tok += 5;
			if(strcmp(tok, "create") == 0)
				a->type = Acreate;
			else if(strcmp(tok, "delete") == 0)
				a->type = Adelete;
			else if(strcmp(tok, "patch") == 0)
				a->type = Apatch;
		} else if(strncmp(tok, "path:", 5) == 0){
			a->path = strdup(tok + 5);
		}
	}

	if(a->type < 0 || a->path == nil){
		free(a->path);
		a->path = nil;
		return 0;
	}

	if(a->path[0] == '/' || strstr(a->path, "..") != nil){
		fprint(2, "action: refusing unsafe path: %s\n", a->path);
		free(a->path);
		a->path = nil;
		return 0;
	}
	return 1;
}

/*
 * Find the first line at or after p whose non-whitespace content
 * starts with prefix, allowing leading whitespace and backticks.
 * If solp is non-nil, sets *solp to the true start of the line.
 * Returns pointer to the prefix within the line, or nil.
 *
 * Unlike findend(), this allows trailing content after the prefix
 * (e.g. "<<<ACTION file:create path:foo.c").
 */
static char*
findline(char *p, char *prefix, char **solp)
{
	int pfxlen, c;
	char *sol, *q;

	pfxlen = strlen(prefix);
	while(*p != '\0'){
		sol = p;
		q = p;
		c = *(uchar*)q;
		while(c == ' ' || c == '\t' || c == '\r' || c == '`' || c == '~'){
			q++;
			c = *(uchar*)q;
		}
		if(strncmp(q, prefix, pfxlen) == 0){
			if(solp != nil)
				*solp = sol;
			return q;
		}
		p = strchr(sol, '\n');
		if(p == nil)
			break;
		p++;
	}
	return nil;
}

/*
 * Find the end marker. Only matches lines where the marker
 * is the sole significant content (ignoring whitespace and
 * backticks around it). This prevents matching body lines
 * that merely mention the marker.
 */
static char*
findend(char *p, char *prefix, char **solp)
{
	int pfxlen, c;
	char *sol, *q, *after;

	pfxlen = strlen(prefix);
	while(*p != '\0'){
		sol = p;
		q = p;
		c = *(uchar*)q;
		while(c == ' ' || c == '\t' || c == '\r' || c == '`' || c == '~'){
			q++;
			c = *(uchar*)q;
		}
		if(strncmp(q, prefix, pfxlen) == 0){
			after = q + pfxlen;
			while(*after == ' ' || *after == '\t' || *after == '\r' || *after == '`' || *after == '~')
				after++;
			if(*after == '\n' || *after == '\0'){
				if(solp != nil)
					*solp = sol;
				return q;
			}
		}
		p = strchr(sol, '\n');
		if(p == nil)
			break;
		p++;
	}
	return nil;
}

static char*
nextline(char *p)
{
	while(*p != '\0' && *p != '\n')
		p++;
	if(*p == '\n')
		p++;
	return p;
}

Action*
parseactions(char *reply)
{
	Action *head, *tail, *a;
	char *p, *hdr, *hdrend, *bodystart, *end;
	char *hdrcopy;
	int nfound, nparsed;
	char *sol;

	head = nil;
	tail = nil;
	p = reply;

	nfound = 0;
	nparsed = 0;
	for(;;){
		p = findline(p, actionstart, nil);
		if(p == nil)
			break;

		hdr = p + strlen(actionstart);
		hdrend = strchr(hdr, '\n');
		if(hdrend == nil)
			break;

		bodystart = hdrend + 1;
		nfound++;

		end = findend(bodystart, actionend, &sol);
		if(end == nil)
			break;

		hdrcopy = malloc(hdrend - hdr + 1);
		if(hdrcopy == nil)
			sysfatal("malloc: %r");
		memmove(hdrcopy, hdr, hdrend - hdr);
		hdrcopy[hdrend - hdr] = '\0';

		a = mallocz(sizeof *a, 1);
		if(a == nil)
			sysfatal("malloc: %r");

		if(!parsehdr(a, hdrcopy)){
			free(hdrcopy);
			free(a);
			p = nextline(end);
			continue;
			/* header parse failed, skip */
		}
		free(hdrcopy);

		/* body ends at start of the line containing the end marker */
		if(sol > bodystart){
			int blen = sol - bodystart;
			while(blen > 0 && bodystart[blen-1] == '\n')
				blen--;
			if(blen > 0){
				a->body = malloc(blen + 1);
				if(a->body == nil)
					sysfatal("malloc: %r");
				memmove(a->body, bodystart, blen);
				a->body[blen] = '\0';
			}
		}

		a->next = nil;
		if(tail == nil)
			head = a;
		else
			tail->next = a;
		tail = a;
		nparsed++;

		p = nextline(end);
	}
	return head;
}

void
freeactions(Action *a)
{
	Action *next;

	while(a != nil){
		next = a->next;
		free(a->path);
		free(a->body);
		free(a);
		a = next;
	}
}

static int
countlines(char *s)
{
	int n;

	if(s == nil || *s == '\0')
		return 0;
	n = 1;
	while(*s != '\0'){
		if(*s == '\n')
			n++;
		s++;
	}
	return n;
}

void
showaction(Action *a, int n)
{
	char *typ;

	switch(a->type){
	case Acreate: typ = "create"; break;
	case Adelete: typ = "delete"; break;
	case Apatch: typ = "patch"; break;
	default: typ = "???"; break;
	}

	if(a->type == Acreate)
		fprint(2, "  [%d] %s %s (%d lines)\n",
			n, typ, a->path, countlines(a->body));
	else if(a->type == Apatch)
		fprint(2, "  [%d] %s %s (%d lines diff)\n",
			n, typ, a->path, countlines(a->body));
	else
		fprint(2, "  [%d] %s %s\n", n, typ, a->path);
}

void
mkparents(char *path)
{
	char *p, *s;
	int fd;

	s = strdup(path);
	if(s == nil)
		sysfatal("strdup: %r");

	for(p = s; *p != '\0'; p++){
		if(*p == '/' && p != s){
			*p = '\0';
			fd = create(s, OREAD, DMDIR|0777);
			if(fd >= 0)
				close(fd);
			*p = '/';
		}
	}
	free(s);
}

static char**
splitlines(char *s, int *nout)
{
	char **lines;
	int n, cap;
	char *p;

	cap = 128;
	n = 0;
	lines = malloc(cap * sizeof(char*));
	if(lines == nil)
		sysfatal("malloc: %r");
	for(p = s; *p != '\0'; n++){
		if(n >= cap){
			cap *= 2;
			lines = realloc(lines, cap * sizeof(char*));
			if(lines == nil)
				sysfatal("realloc: %r");
		}
		lines[n] = p;
		p = strchr(p, '\n');
		if(p == nil)
			p = s + strlen(s);
		else
			*p++ = '\0';
	}
	*nout = n;
	return lines;
}

typedef struct Hunk Hunk;
struct Hunk {
	int oldstart;
	int oldcount;
	int newstart;
	int newcount;
	char **lines;
	int nlines;
	int alines;
};

static int
wsmatch(char *a, char *b)
{
	if(strcmp(a, b) == 0)
		return 1;

	while(*a != '\0' && *b != '\0'){
		if((*a == ' ' || *a == '\t') && (*b == ' ' || *b == '\t')){
			while(*a == ' ' || *a == '\t')
				a++;
			while(*b == ' ' || *b == '\t')
				b++;
			continue;
		}
		if(*a != *b)
			return 0;
		a++;
		b++;
	}
	while(*a == ' ' || *a == '\t')
		a++;
	while(*b == ' ' || *b == '\t')
		b++;
	return *a == '\0' && *b == '\0';
}

static int
parsehunkhdr(char *line, Hunk *h)
{
	char *p;
	p = line;
	if(p[0] != '@' || p[1] != '@')
		return 0;
	p += 2;
	while(*p == ' ') p++;
	if(*p != '-') return 0;
	p++;
	h->oldstart = atoi(p);
	while(*p >= '0' && *p <= '9') p++;
	if(*p == ','){
		p++;
		h->oldcount = atoi(p);
		while(*p >= '0' && *p <= '9') p++;
	} else
		h->oldcount = 1;
	while(*p == ' ') p++;
	if(*p != '+') return 0;
	p++;
	h->newstart = atoi(p);
	while(*p >= '0' && *p <= '9') p++;
	if(*p == ','){
		p++;
		h->newcount = atoi(p);
	} else
		h->newcount = 1;
	return 1;
}

static int
matchcontext(Hunk *h, char **orig, int norig, int pos)
{
	int i, o;
	o = pos;
	for(i = 0; i < h->nlines; i++){
		if(h->lines[i][0] == '+')
			continue;
		if(o >= norig)
			return 0;
		if(!wsmatch(h->lines[i] + 1, orig[o]))
			return 0;
		o++;
	}
	return 1;
}

typedef struct Outbuf Outbuf;
struct Outbuf {
	int fd;
	char *buf;
	int len;
	int cap;
};

static Outbuf*
outbufnew(int fd)
{
	Outbuf *o;

	o = mallocz(sizeof *o, 1);
	if(o == nil)
		sysfatal("malloc: %r");
	o->fd = fd;
	o->cap = 65536;
	o->buf = malloc(o->cap);
	if(o->buf == nil)
		sysfatal("malloc: %r");
	return o;
}

static void
outbufflush(Outbuf *o)
{
	if(o->len > 0 && o->fd >= 0){
		write(o->fd, o->buf, o->len);
		o->len = 0;
	}
}

static void
outbufwrite(Outbuf *o, char *s, int n)
{
	if(o->len + n > o->cap){
		if(o->fd < 0){
			while(o->len + n > o->cap)
				o->cap = o->cap ? o->cap * 2 : 65536;
			o->buf = realloc(o->buf, o->cap);
			if(o->buf == nil)
				sysfatal("realloc: %r");
		} else {
			outbufflush(o);
		}
		if(o->fd >= 0 && n > o->cap){
			write(o->fd, s, n);
			return;
		}
	}
	memmove(o->buf + o->len, s, n);
	o->len += n;
}

static void
outbuffree(Outbuf *o)
{
	outbufflush(o);
	free(o->buf);
	free(o);
}

static void
writeline(Outbuf *o, char *line)
{
	outbufwrite(o, line, strlen(line));
	outbufwrite(o, "\n", 1);
}

static int
applyhunk(Outbuf *ob, Hunk *h, char **orig, int norig, int *origpos)
{
	int target, fuzz, pos, i, o;
	char prefix;
	char *content;

	target = h->oldstart - 1;
	pos = -1;

	for(fuzz = 0; fuzz <= 3 && pos < 0; fuzz++){
		if(target - fuzz >= *origpos && target - fuzz >= 0)
			if(matchcontext(h, orig, norig, target - fuzz)){
				pos = target - fuzz;
				break;
			}
		if(fuzz > 0 && target + fuzz >= *origpos && target + fuzz < norig)
			if(matchcontext(h, orig, norig, target + fuzz)){
				pos = target + fuzz;
				break;
			}
	}

	if(pos < 0){
		for(i = *origpos; i < norig; i++){
			if(matchcontext(h, orig, norig, i)){
				pos = i;
				break;
			}
		}
	}

	if(pos < 0)
		return 0;
	while(*origpos < pos){
		writeline(ob, orig[*origpos]);
		(*origpos)++;
	}
	o = pos;
	for(i = 0; i < h->nlines; i++){
		prefix = h->lines[i][0];
		content = h->lines[i] + 1;
		if(prefix == ' '){
			if(o < norig)
				writeline(ob, orig[o]);
			else
				writeline(ob, content);
			o++;
		} else if(prefix == '-'){
			o++;
		} else if(prefix == '+'){
			writeline(ob, content);
		}
	}
	*origpos = o;
	return 1;
}

static void
hunkadd(Hunk *h, char *line)
{
	if(h->nlines >= h->alines){
		h->alines = h->alines ? h->alines * 2 : 16;
		h->lines = realloc(h->lines,
			h->alines * sizeof(char*));
		if(h->lines == nil)
			sysfatal("realloc: %r");
	}
	h->lines[h->nlines++] = line;
}

static void
freehunks(Hunk *hunks, int nhunks)
{
	int i;
	for(i = 0; i < nhunks; i++)
		free(hunks[i].lines);
	free(hunks);
}

static int
applypatch(Action *a)
{
	char *orig, *diffbuf;
	char **olines, **dlines;
	int norig, ndiff;
	Hunk *hunks;
	int nhunks, ahunks;
	int i, fd;
	int origpos;
	Outbuf *ob;
	char **synth;
	int nsynth, asynth;

	if(a->body == nil){
		fprint(2, "action: patch %s: no diff\n", a->path);
		return 0;
	}
	fd = open(a->path, OREAD);
	if(fd < 0){
		fprint(2, "action: patch %s: %r\n", a->path);
		return 0;
	}
	orig = readfile(fd);
	close(fd);
	if(orig == nil){
		fprint(2, "action: patch %s: read: %r\n", a->path);
		return 0;
	}

	diffbuf = strdup(a->body);
	if(diffbuf == nil)
		sysfatal("strdup: %r");

	olines = splitlines(orig, &norig);
	dlines = splitlines(diffbuf, &ndiff);

	nhunks = 0;
	ahunks = 8;
	hunks = malloc(ahunks * sizeof(Hunk));
	if(hunks == nil)
		sysfatal("malloc: %r");

	nsynth = 0;
	asynth = 16;
	synth = malloc(asynth * sizeof(char*));
	if(synth == nil)
		sysfatal("malloc: %r");

	for(i = 0; i < ndiff; i++){
		/* skip --- and +++ file headers but not removal/addition lines */
		if(strncmp(dlines[i], "--- ", 4) == 0)
			continue;
		if(strncmp(dlines[i], "+++ ", 4) == 0)
			continue;
		if(dlines[i][0] == '\\')
			continue;

		if(strncmp(dlines[i], "@@", 2) == 0){
			if(nhunks >= ahunks){
				ahunks *= 2;
				hunks = realloc(hunks, ahunks * sizeof(Hunk));
				if(hunks == nil)
					sysfatal("realloc: %r");
			}
			memset(&hunks[nhunks], 0, sizeof(Hunk));
			if(!parsehunkhdr(dlines[i], &hunks[nhunks])){
				fprint(2, "action: patch %s: bad hunk header: %s\n",
					a->path, dlines[i]);
				goto fail;
			}
			nhunks++;
			continue;
		}

		if(nhunks == 0)
			continue;

		if(dlines[i][0] == '\0'){
			char *s = strdup(" ");
			if(s == nil)
				sysfatal("strdup: %r");
			if(nsynth >= asynth){
				asynth *= 2;
				synth = realloc(synth, asynth * sizeof(char*));
				if(synth == nil)
					sysfatal("realloc: %r");
			}
			synth[nsynth++] = s;
			hunkadd(&hunks[nhunks - 1], s);
			continue;
		}

		if(dlines[i][0] == ' ' || dlines[i][0] == '-' || dlines[i][0] == '+'){
			hunkadd(&hunks[nhunks - 1], dlines[i]);
		}
	}

	if(nhunks == 0){
		fprint(2, "action: patch %s: no hunks found\n", a->path);
		goto fail;
	}

	/* apply to memory buffer first so we don't destroy the file on failure */
	ob = outbufnew(-1);
	origpos = 0;
	for(i = 0; i < nhunks; i++){
		if(!applyhunk(ob, &hunks[i], olines, norig, &origpos)){
			fprint(2, "action: patch %s: hunk %d/%d failed at line %d\n",
				a->path, i + 1, nhunks, hunks[i].oldstart);
			outbuffree(ob);
			goto fail;
		}
	}
	while(origpos < norig){
		writeline(ob, olines[origpos]);
		origpos++;
	}

	/* all hunks succeeded, now write the file */
	fd = create(a->path, OWRITE, 0666);
	if(fd < 0){
		fprint(2, "action: patch %s: create: %r\n", a->path);
		outbuffree(ob);
		goto fail;
	}
	if(ob->len > 0)
		write(fd, ob->buf, ob->len);
	outbuffree(ob);
	close(fd);
	fprint(2, "  patched %s (%d hunks)\n", a->path, nhunks);

	free(olines);
	free(orig);
	free(dlines);
	free(diffbuf);
	freehunks(hunks, nhunks);
	for(i = 0; i < nsynth; i++)
		free(synth[i]);
	free(synth);
	return 1;

fail:
	free(olines);
	free(orig);
	free(dlines);
	free(diffbuf);
	freehunks(hunks, nhunks);
	for(i = 0; i < nsynth; i++)
		free(synth[i]);
	free(synth);
	return 0;
}

int
applyaction(Action *a)
{
	int fd;
	long n;

	switch(a->type){
	case Acreate:
		if(a->body == nil){
			fprint(2, "action: create %s: no body\n", a->path);
			return 0;
		}
		mkparents(a->path);
		fd = create(a->path, OWRITE, 0666);
		if(fd < 0){
			fprint(2, "action: create %s: %r\n", a->path);
			return 0;
		}
		n = strlen(a->body);
		if(write(fd, a->body, n) != n){
			fprint(2, "action: write %s: %r\n", a->path);
			close(fd);
			return 0;
		}
		if(n > 0 && a->body[n-1] != '\n')
			write(fd, "\n", 1);
		close(fd);
		fprint(2, "  created %s\n", a->path);
		return 1;
	case Adelete:
		if(remove(a->path) < 0){
			fprint(2, "action: delete %s: %r\n", a->path);
			return 0;
		}
		fprint(2, "  deleted %s\n", a->path);
		return 1;
	case Apatch:
		return applypatch(a);
	default:
		fprint(2, "action: unknown type %d\n", a->type);
		return 0;
	}
}

char*
stripactions(char *reply)
{
	char *out, *p, *q, *start, *end;
	int len, olen;
	char *solstart, *solend;

	len = strlen(reply);
	out = malloc(len + 1);
	if(out == nil)
		sysfatal("malloc: %r");
	olen = 0;
	p = reply;
	for(;;){
		start = findline(p, actionstart, &solstart);
		if(start == nil){
			q = p + strlen(p);
			memmove(out + olen, p, q - p);
			olen += q - p;
			break;
		}
		if(solstart > p){
			memmove(out + olen, p, solstart - p);
			olen += solstart - p;
		}
		end = findend(start + strlen(actionstart), actionend, &solend);
		if(end == nil){
			break;
		}
		p = nextline(end);
	}
	out[olen] = '\0';
	while(olen > 0 && (out[olen-1] == '\n' || out[olen-1] == ' '))
		out[--olen] = '\0';
	return out;
}
