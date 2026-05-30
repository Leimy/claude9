#include <u.h>
#include <libc.h>
#include "claude.h"

/*
 * In-process unified diff applier.
 *
 * Instead of shelling out to /bin/patch, we read the target file
 * into memory, parse the diff into hunks, locate each hunk's
 * context in the file (with fuzz), splice in the changes, and
 * write the result back.  This gives us full control over error
 * reporting -- the result string says exactly which hunks matched
 * and where, or why they failed.
 *
 * LLM-tolerance features:
 *   - Line numbers in @@ headers are hints, not gospel
 *   - Lines without a leading ' ', '-', or '+' inside a hunk
 *     body are treated as context (missing-space fix)
 *   - Whitespace is normalized when matching context:
 *     tabs and spaces are equivalent, runs are collapsed,
 *     leading/trailing whitespace is ignored
 *   - Fuzz: we search up to 500 lines away from the hint
 */

enum {
	Maxfuzz = 500,
};

/* ---- line array ---- */

typedef struct Lines Lines;
struct Lines {
	char **v;	/* array of line strings (each malloc'd) */
	int n;		/* count */
	int cap;
};

static void
linesinit(Lines *ls)
{
	ls->v = nil;
	ls->n = 0;
	ls->cap = 0;
}

static void
linesfree(Lines *ls)
{
	int i;
	for(i = 0; i < ls->n; i++)
		free(ls->v[i]);
	free(ls->v);
	ls->v = nil;
	ls->n = 0;
	ls->cap = 0;
}

static void
linesadd(Lines *ls, char *s)
{
	if(ls->n >= ls->cap){
		ls->cap = ls->cap ? ls->cap * 2 : 64;
		ls->v = erealloc(ls->v, ls->cap * sizeof ls->v[0]);
	}
	ls->v[ls->n++] = estrdup(s);
}

/*
 * Split text into lines.  Each line includes its trailing \n
 * if present.  A final line without \n is still included.
 */
static void
splitlines(char *text, Lines *ls)
{
	char *p, *sol;

	linesinit(ls);
	for(p = text; *p != '\0'; ){
		sol = p;
		while(*p != '\0' && *p != '\n')
			p++;
		if(*p == '\n')
			p++;
		/* sol..p is one line */
		{
			int len = p - sol;
			char *tmp = emalloc(len + 1);
			memmove(tmp, sol, len);
			tmp[len] = '\0';
			linesadd(ls, tmp);
			free(tmp);
		}
	}
}

/*
 * Join lines back into a single malloc'd string.
 */
static char*
joinlines(Lines *ls)
{
	int i, total;
	char *buf, *p;

	total = 0;
	for(i = 0; i < ls->n; i++)
		total += strlen(ls->v[i]);
	buf = emalloc(total + 1);
	p = buf;
	for(i = 0; i < ls->n; i++){
		int len = strlen(ls->v[i]);
		memmove(p, ls->v[i], len);
		p += len;
	}
	*p = '\0';
	return buf;
}

/* ---- hunk parsing ---- */

typedef struct DiffLine DiffLine;
struct DiffLine {
	int op;		/* ' ', '-', '+' */
	char *text;	/* line content (after the op char, malloc'd) */
};

typedef struct Hunk Hunk;
struct Hunk {
	int oldstart;	/* hint line number in old file (1-based) */
	int newstart;	/* hint line number in new file (1-based) */
	DiffLine *lines;
	int nlines;
	int lcap;
};

static void
hunkfree(Hunk *h)
{
	int i;
	for(i = 0; i < h->nlines; i++)
		free(h->lines[i].text);
	free(h->lines);
}

static void
hunkadd(Hunk *h, int op, char *text)
{
	if(h->nlines >= h->lcap){
		h->lcap = h->lcap ? h->lcap * 2 : 32;
		h->lines = erealloc(h->lines, h->lcap * sizeof h->lines[0]);
	}
	h->lines[h->nlines].op = op;
	h->lines[h->nlines].text = estrdup(text);
	h->nlines++;
}

/*
 * Strip trailing whitespace (spaces, tabs, \r, \n) from s in place.
 * Returns s for convenience.
 */
static char*
trimright(char *s)
{
	int n;

	n = strlen(s);
	while(n > 0 && (s[n-1] == ' ' || s[n-1] == '\t'
	             || s[n-1] == '\r' || s[n-1] == '\n'))
		n--;
	s[n] = '\0';
	return s;
}

/*
 * Normalize whitespace for comparison: collapse each run of
 * tabs/spaces into a single space, strip leading and trailing
 * whitespace.  Writes into caller-supplied buf of size bufsz.
 */
static void
normws(char *s, char *buf, int bufsz)
{
	char *d, *end;

	d = buf;
	end = buf + bufsz - 1;
	/* skip leading whitespace */
	while(*s == ' ' || *s == '\t')
		s++;
	while(*s != '\0' && d < end){
		if(*s == ' ' || *s == '\t'){
			if(d > buf && d[-1] != ' ')
				*d++ = ' ';
			s++;
		} else {
			*d++ = *s++;
		}
	}
	/* strip trailing space */
	if(d > buf && d[-1] == ' ')
		d--;
	*d = '\0';
}

/*
 * Compare two strings with whitespace tolerance: ignore
 * trailing whitespace, and treat runs of tabs/spaces as
 * equivalent (so tab-indented file lines match space-indented
 * diff context).
 */
static int
trimcmp(char *a, char *b)
{
	char abuf[4096], bbuf[4096];

	normws(a, abuf, sizeof abuf);
	normws(b, bbuf, sizeof bbuf);
	return strcmp(abuf, bbuf);
}

/*
 * Parse hunks from a unified diff string.
 * Returns an array of Hunk (caller frees each with hunkfree,
 * then frees the array).  *nhunks is set to the count.
 */
static Hunk*
parsehunks(char *diff, int *nhunks)
{
	Hunk *hunks;
	int hcap, nh;
	char *p, *sol, *eol;
	int inhunk;
	Hunk *cur;

	hunks = nil;
	hcap = 0;
	nh = 0;
	inhunk = 0;
	cur = nil;

	for(p = diff; *p != '\0'; ){
		sol = p;
		eol = strchr(p, '\n');
		if(eol != nil)
			eol++;
		else
			eol = p + strlen(p);

		/* check for @@ header */
		if(strncmp(sol, "@@", 2) == 0){
			char *q;
			int os, ns;

			/* grow array */
			if(nh >= hcap){
				hcap = hcap ? hcap * 2 : 8;
				hunks = erealloc(hunks, hcap * sizeof hunks[0]);
			}
			cur = &hunks[nh++];
			memset(cur, 0, sizeof *cur);

			/* parse @@ -OLD,CNT +NEW,CNT @@ */
			q = sol + 2;
			while(*q == ' ') q++;
			if(*q == '-') q++;
			os = atoi(q);
			if(os < 1) os = 1;
			ns = os;	/* default: same as old start */
			q = strchr(q, '+');
			if(q != nil){
				q++;
				ns = atoi(q);
				if(ns < 1) ns = 1;
			}
			cur->oldstart = os;
			cur->newstart = ns;
			inhunk = 1;
			p = eol;
			continue;
		}

		/* skip --- and +++ headers */
		if(strncmp(sol, "--- ", 4) == 0
		|| strncmp(sol, "+++ ", 4) == 0){
			p = eol;
			continue;
		}

		/* skip "\ No newline" lines */
		if(sol[0] == '\\'){
			p = eol;
			continue;
		}

		if(!inhunk || cur == nil){
			p = eol;
			continue;
		}

		/* hunk body line */
		{
			int linelen = eol - sol;
			char *tmp = emalloc(linelen + 1);
			memmove(tmp, sol, linelen);
			tmp[linelen] = '\0';

			if(tmp[0] == ' ' || tmp[0] == '-' || tmp[0] == '+'){
				hunkadd(cur, tmp[0], tmp + 1);
			} else if(tmp[0] == '\n' || tmp[0] == '\0'){
				/*
				 * Blank line in hunk -- this is context.
				 * Some diffs have bare newlines as context.
				 */
				hunkadd(cur, ' ', "\n");
			} else {
				/*
				 * Line without prefix -- treat as context
				 * with the full line as content (LLM forgot
				 * the leading space).
				 */
				hunkadd(cur, ' ', tmp);
			}
			free(tmp);
		}
		p = eol;
	}

	*nhunks = nh;
	return hunks;
}

/* ---- context matching ---- */

/*
 * Extract the "old" lines from a hunk (context + removed).
 * Returns count; fills oldlines[] with pointers into hunk
 * (not copies -- do not free).
 */
static int
hunkold(Hunk *h, char **oldlines, int maxold)
{
	int i, n;

	n = 0;
	for(i = 0; i < h->nlines && n < maxold; i++){
		if(h->lines[i].op == ' ' || h->lines[i].op == '-')
			oldlines[n++] = h->lines[i].text;
	}
	return n;
}

/*
 * Try to match the old-lines of a hunk starting at position
 * 'pos' in the file lines array.  Returns 1 if all old lines
 * match (ignoring trailing whitespace), 0 otherwise.
 */
static int
trymatch(Lines *file, int pos, char **oldlines, int nold)
{
	int i;

	if(pos < 0 || pos + nold > file->n)
		return 0;
	for(i = 0; i < nold; i++){
		if(trimcmp(file->v[pos + i], oldlines[i]) != 0)
			return 0;
	}
	return 1;
}

/*
 * Search for the old-lines of a hunk in the file, starting near
 * 'hint' (0-based) and scanning outward.  Returns the matching
 * position (0-based) or -1.
 */
static int
findmatch(Lines *file, int hint, char **oldlines, int nold)
{
	int d;

	if(nold == 0)
		return hint;  /* pure-add hunk matches at hint */

	if(hint < 0) hint = 0;
	if(hint >= file->n) hint = file->n - 1;
	if(hint < 0) return -1;

	/* try exact position first */
	if(trymatch(file, hint, oldlines, nold))
		return hint;

	/* scan outward */
	for(d = 1; d <= Maxfuzz; d++){
		if(hint - d >= 0 && trymatch(file, hint - d, oldlines, nold))
			return hint - d;
		if(trymatch(file, hint + d, oldlines, nold))
			return hint + d;
	}
	return -1;
}

/* ---- hunk application ---- */

/*
 * Apply a single hunk at position 'pos' in the file lines.
 * Removes old lines (context+removed) and inserts new lines
 * (context+added) in their place.
 * For context lines (op == ' '), the file's original text is
 * preserved rather than the diff's version, avoiding silent
 * whitespace corruption from tab/space mismatches in the diff.
 *
 * Returns the line-count delta to adjust subsequent hint positions.
 */
static int
applyhunk(Lines *file, int pos, Hunk *h)
{
	int nold, nnew, delta, i, j;
	char **newlines;
	int newcap, newn;

	/* count old and new lines */
	nold = 0;
	nnew = 0;
	for(i = 0; i < h->nlines; i++){
		if(h->lines[i].op == ' '){
			nold++;
			nnew++;
		} else if(h->lines[i].op == '-'){
			nold++;
		} else if(h->lines[i].op == '+'){
			nnew++;
		}
	}

	/*
	 * Build new lines array for this region.
	 * For context lines we use the file's original text
	 * (at the corresponding old-line position) so that we
	 * don't corrupt indentation when the diff has spaces
	 * but the file has tabs.
	 */
	newcap = nnew + 1;
	newlines = emalloc(newcap * sizeof newlines[0]);
	newn = 0;
	{
	int oldidx;		/* tracks position in old file region */
	oldidx = 0;
	for(i = 0; i < h->nlines; i++){
		if(h->lines[i].op == ' '){
			/*
			 * Strdup immediately: the free loop below will
			 * release all old file lines including context.
			 */
			newlines[newn++] = estrdup(file->v[pos + oldidx]);
			oldidx++;
		} else if(h->lines[i].op == '-'){
			oldidx++;
		} else if(h->lines[i].op == '+'){
			newlines[newn++] = estrdup(h->lines[i].text);
		}
	}
	}

	/* free old lines being removed */
	for(i = pos; i < pos + nold && i < file->n; i++)
		free(file->v[i]);

	delta = nnew - nold;

	if(delta > 0){
		/* need more space -- grow and shift right */
		while(file->n + delta > file->cap){
			file->cap = file->cap ? file->cap * 2 : 64;
			file->v = erealloc(file->v, file->cap * sizeof file->v[0]);
		}
		memmove(file->v + pos + nnew,
			file->v + pos + nold,
			(file->n - pos - nold) * sizeof file->v[0]);
	} else if(delta < 0){
		/* shift left */
		memmove(file->v + pos + nnew,
			file->v + pos + nold,
			(file->n - pos - nold) * sizeof file->v[0]);
	}
	file->n += delta;

	/* place new lines -- newlines[] entries are already malloc'd */
	for(j = 0; j < newn; j++)
		file->v[pos + j] = newlines[j];

	free(newlines);
	return delta;
}

/* ---- status string builder ---- */

typedef struct Sbuf Sbuf;
struct Sbuf {
	char *s;
	int len;
	int cap;
};

static void
sinit(Sbuf *sb)
{
	sb->cap = 256;
	sb->s = emalloc(sb->cap);
	sb->s[0] = '\0';
	sb->len = 0;
}

static void
sappend(Sbuf *sb, char *fmt, ...)
{
	va_list arg;
	char buf[1024];
	int n;

	va_start(arg, fmt);
	n = vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	while(sb->len + n + 1 > sb->cap){
		sb->cap *= 2;
		sb->s = erealloc(sb->s, sb->cap);
	}
	memmove(sb->s + sb->len, buf, n);
	sb->len += n;
	sb->s[sb->len] = '\0';
}

/* ---- main entry point ---- */

/*
 * applydiff: apply a unified diff to the file at path.
 *
 * Returns a malloc'd status string:
 *   "patched <path> (N hunks)"  on success
 *   "error: ..."                on failure
 *
 * Never returns nil.
 */
char*
applydiff(char *path, char *diff)
{
	int fd, n, nhunks, i, pos, applied, failed;
	int adjustment;
	char *data, *result, *newdata;
	char **oldlines;
	int nold;
	Lines file;
	Hunk *hunks;
	Sbuf detail;

	if(path == nil || path[0] == '\0')
		return esmprint("error: no file path specified");
	if(diff == nil || diff[0] == '\0')
		return esmprint("error: empty diff");

	hunks = parsehunks(diff, &nhunks);
	if(nhunks == 0){
		free(hunks);
		return esmprint("error: no hunks found in diff");
	}

	/*
	 * Read existing file.  If the file doesn't exist, start
	 * with empty content (the diff might be creating it via
	 * --- /dev/null).
	 */
	mkparents(path);
	fd = open(path, OREAD);
	if(fd >= 0){
		data = readfile(fd);
		close(fd);
		if(data == nil)
			data = estrdup("");
	} else {
		data = estrdup("");
	}

	splitlines(data, &file);
	free(data);

	sinit(&detail);
	applied = 0;
	failed = 0;
	adjustment = 0;  /* cumulative line shift from earlier hunks */

	oldlines = emalloc(16384 * sizeof oldlines[0]);

	for(i = 0; i < nhunks; i++){
		Hunk *h = &hunks[i];
		int hint;

		nold = hunkold(h, oldlines, 16384);
		hint = h->oldstart - 1 + adjustment;  /* 0-based */

		pos = findmatch(&file, hint, oldlines, nold);
		if(pos < 0){
			failed++;
			sappend(&detail, "hunk %d/%d FAILED: "
				"could not find context near line %d",
				i + 1, nhunks, h->oldstart);
			if(nold > 0){
				char preview[80];
				int plen;
				snprint(preview, sizeof preview, "%s",
					oldlines[0]);
				trimright(preview);
				plen = strlen(preview);
				if(plen > 60){
					preview[60] = '\0';
					sappend(&detail, " (looking for: \"%.60s...\")", preview);
				} else
					sappend(&detail, " (looking for: \"%s\")", preview);
			}
			sappend(&detail, "\n");
		} else {
			int delta;
			delta = applyhunk(&file, pos, h);
			adjustment += delta;
			applied++;
			if(pos != h->oldstart - 1)
				sappend(&detail, "hunk %d/%d applied at line %d "
					"(offset %d from hint %d)\n",
					i + 1, nhunks,
					pos + 1,
					pos + 1 - h->oldstart,
					h->oldstart);
		}
	}

	free(oldlines);

	if(failed > 0 && applied == 0){
		/* total failure -- don't write anything */
		result = esmprint("error: all %d hunks failed for %s\n%s",
			nhunks, path, detail.s);
		free(detail.s);
		linesfree(&file);
		for(i = 0; i < nhunks; i++)
			hunkfree(&hunks[i]);
		free(hunks);
		return result;
	}

	/* write result back */
	newdata = joinlines(&file);
	linesfree(&file);

	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		result = esmprint("error: create %s: %r", path);
		free(newdata);
		free(detail.s);
		for(i = 0; i < nhunks; i++)
			hunkfree(&hunks[i]);
		free(hunks);
		return result;
	}
	n = strlen(newdata);
	if(write(fd, newdata, n) != n){
		close(fd);
		result = esmprint("error: write %s: %r", path);
		free(newdata);
		free(detail.s);
		for(i = 0; i < nhunks; i++)
			hunkfree(&hunks[i]);
		free(hunks);
		return result;
	}
	close(fd);
	free(newdata);

	if(failed > 0)
		result = esmprint("patched %s (%d/%d hunks applied, %d failed)\n%s",
			path, applied, nhunks, failed, detail.s);
	else
		result = esmprint("patched %s (%d hunks)", path, applied);

	free(detail.s);
	for(i = 0; i < nhunks; i++)
		hunkfree(&hunks[i]);
	free(hunks);
	return result;
}
