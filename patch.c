#include <u.h>
#include <libc.h>
#include "claude.h"

/*
 * Fuzzy unified-diff applier for claude9fs.
 *
 * Parses a unified diff into hunks, then applies each hunk to
 * the target file by searching for context lines near the
 * hinted position.  Tolerates:
 *   - missing or malformed --- / +++ headers
 *   - bare @@ with no line numbers
 *   - off-by-one (or more) line numbers
 *   - whitespace-only differences in context lines
 *
 * The single entry point is applydiff(path, diff), declared
 * in claude.h.
 */

enum {
	Maxhunks = 4096,
	Fuzzlines = 250,	/* how far from the hint to search */
};

typedef struct Patch Patch;
typedef struct Hunk Hunk;

/* a single line from the diff: context (' '), add ('+'), or remove ('-') */
typedef struct Dline Dline;
struct Dline {
	int op;		/* ' ', '+', '-' */
	char *text;	/* line content without the prefix */
};

struct Hunk {
	int oldstart;	/* hinted old-file start line (1-based), 0 if unknown */
	Dline *lines;
	int nlines;
};

struct Patch {
	Hunk hunks[Maxhunks];
	int nhunks;
};

/*
 * Split a string into lines.  Returns a malloc'd array of
 * malloc'd strings; *np is set to the count.  A trailing
 * newline on the last line is preserved in the line content.
 * An empty input produces nlines==0.
 */
static char**
splitlines(char *s, int *np)
{
	char **lines;
	int n, cap;
	char *p, *eol;

	cap = 256;
	lines = malloc(cap * sizeof(char*));
	if(lines == nil)
		sysfatal("malloc: %r");
	n = 0;

	p = s;
	while(*p != '\0'){
		eol = strchr(p, '\n');
		if(eol != nil)
			eol++;	/* include the newline */
		else
			eol = p + strlen(p);
		if(n >= cap){
			cap *= 2;
			lines = realloc(lines, cap * sizeof(char*));
			if(lines == nil)
				sysfatal("realloc: %r");
		}
		lines[n] = malloc(eol - p + 1);
		if(lines[n] == nil)
			sysfatal("malloc: %r");
		memmove(lines[n], p, eol - p);
		lines[n][eol - p] = '\0';
		n++;
		p = eol;
	}
	*np = n;
	return lines;
}

static void
freelines(char **lines, int n)
{
	int i;
	for(i = 0; i < n; i++)
		free(lines[i]);
	free(lines);
}

/*
 * Compare two strings for equality, ignoring differences that
 * are only in whitespace (spaces and tabs).  Both strings may
 * or may not have trailing newlines.
 */
static int
wsmatch(char *a, char *b)
{
	while(*a != '\0' && *b != '\0'){
		if(*a == *b){
			a++;
			b++;
			continue;
		}
		/* if both are whitespace, skip whitespace runs in both */
		if((*a == ' ' || *a == '\t') && (*b == ' ' || *b == '\t')){
			while(*a == ' ' || *a == '\t') a++;
			while(*b == ' ' || *b == '\t') b++;
			continue;
		}
		/* skip trailing newlines for comparison */
		if(*a == '\n' && *(a+1) == '\0' && *b == '\0')
			return 1;
		if(*b == '\n' && *(b+1) == '\0' && *a == '\0')
			return 1;
		return 0;
	}
	/* skip trailing whitespace/newlines */
	while(*a == ' ' || *a == '\t' || *a == '\n') a++;
	while(*b == ' ' || *b == '\t' || *b == '\n') b++;
	return *a == '\0' && *b == '\0';
}

/*
 * Parse the @@ line to extract old-file start line.
 * Accepts: @@ -N,M +O,P @@, @@ -N +O @@, or bare @@.
 * Returns the old start line (1-based), or 0 if unparseable.
 */
static int
parseat(char *line)
{
	char *p;
	int n;

	p = line;
	/* skip leading @@ */
	if(*p != '@') return 0;
	while(*p == '@') p++;
	while(*p == ' ') p++;
	if(*p != '-') return 0;
	p++;
	n = atoi(p);
	if(n <= 0) n = 1;
	return n;
}

/*
 * Parse a unified diff string into a Patch structure.
 * Skips --- and +++ header lines.  Each @@ line starts a new hunk.
 * Lines starting with ' ', '+', '-' are hunk content.
 * Lines without a recognised prefix after a @@ that look like
 * context are treated as context (' ').
 */
static Patch*
parsepatch(char *diff)
{
	Patch *p;
	char **dlines;
	int ndlines, i;
	Hunk *h;
	int dcap;

	p = mallocz(sizeof *p, 1);
	if(p == nil)
		sysfatal("malloc: %r");

	dlines = splitlines(diff, &ndlines);

	h = nil;
	dcap = 0;

	for(i = 0; i < ndlines; i++){
		char *l = dlines[i];

		/* skip --- and +++ headers */
		if(strncmp(l, "--- ", 4) == 0 || strncmp(l, "---\t", 4) == 0)
			continue;
		if(strncmp(l, "+++ ", 4) == 0 || strncmp(l, "+++\t", 4) == 0)
			continue;
		/* also skip "--- a/..." and "+++ b/..." with no tab */
		if(strncmp(l, "---", 3) == 0 && (l[3] == ' ' || l[3] == '\t' || l[3] == '\n' || l[3] == '\0'))
			continue;
		if(strncmp(l, "+++", 3) == 0 && (l[3] == ' ' || l[3] == '\t' || l[3] == '\n' || l[3] == '\0'))
			continue;

		/* @@ starts a new hunk */
		if(strncmp(l, "@@", 2) == 0){
			if(p->nhunks >= Maxhunks)
				break;
			h = &p->hunks[p->nhunks++];
			h->oldstart = parseat(l);
			h->lines = nil;
			h->nlines = 0;
			dcap = 0;
			continue;
		}

		/* skip lines before first hunk */
		if(h == nil)
			continue;

		/* diff content line */
		if(h->nlines >= dcap){
			dcap = dcap ? dcap * 2 : 64;
			h->lines = realloc(h->lines, dcap * sizeof(Dline));
			if(h->lines == nil)
				sysfatal("realloc: %r");
		}

		if(l[0] == ' '){
			h->lines[h->nlines].op = ' ';
			h->lines[h->nlines].text = strdup(l + 1);
		} else if(l[0] == '+'){
			h->lines[h->nlines].op = '+';
			h->lines[h->nlines].text = strdup(l + 1);
		} else if(l[0] == '-'){
			h->lines[h->nlines].op = '-';
			h->lines[h->nlines].text = strdup(l + 1);
		} else if(l[0] == '\\'){
			/* "\ No newline at end of file" -- skip */
			continue;
		} else {
			/* treat unrecognised lines as context */
			h->lines[h->nlines].op = ' ';
			h->lines[h->nlines].text = strdup(l);
		}
		h->nlines++;
	}

	freelines(dlines, ndlines);
	return p;
}

static void
freepatch(Patch *p)
{
	int i, j;
	for(i = 0; i < p->nhunks; i++){
		for(j = 0; j < p->hunks[i].nlines; j++)
			free(p->hunks[i].lines[j].text);
		free(p->hunks[i].lines);
	}
	free(p);
}

/*
 * Extract the context (remove) lines from a hunk: the sequence
 * of ' ' and '-' lines, which represents what should be found
 * in the old file.  Returns arrays of the text and the
 * corresponding op for each line.  *np is set to the count.
 */
static void
hunkcontext(Hunk *h, char ***textp, int **opp, int *np)
{
	int i, n, cap;
	char **text;
	int *op;

	cap = 64;
	text = malloc(cap * sizeof(char*));
	op = malloc(cap * sizeof(int));
	if(text == nil || op == nil)
		sysfatal("malloc: %r");
	n = 0;

	for(i = 0; i < h->nlines; i++){
		if(h->lines[i].op == '+')
			continue;
		if(n >= cap){
			cap *= 2;
			text = realloc(text, cap * sizeof(char*));
			op = realloc(op, cap * sizeof(int));
			if(text == nil || op == nil)
				sysfatal("realloc: %r");
		}
		text[n] = h->lines[i].text;
		op[n] = h->lines[i].op;
		n++;
	}
	*textp = text;
	*opp = op;
	*np = n;
}

/*
 * Try to match the old-file lines of a hunk starting at
 * position pos in the file lines array.  Returns 1 if all
 * context/' ' and '-' lines match (using fuzzy whitespace
 * comparison), 0 otherwise.
 */
static int
trymatch(char **flines, int nflines, int pos, char **ctx, int nctx)
{
	int i;

	if(pos < 0 || pos + nctx > nflines)
		return 0;
	for(i = 0; i < nctx; i++){
		if(!wsmatch(flines[pos + i], ctx[i]))
			return 0;
	}
	return 1;
}

/*
 * Find the best position to apply a hunk.  Searches outward
 * from the hinted position.  Returns the 0-based file line
 * index, or -1 if no match.
 */
static int
findhunk(char **flines, int nflines, Hunk *h, int hint)
{
	char **ctx;
	int *op;
	int nctx, i;
	int pos;

	hunkcontext(h, &ctx, &op, &nctx);
	if(nctx == 0){
		/* pure insertion: use hint */
		free(ctx);
		free(op);
		return hint < nflines ? hint : nflines;
	}

	/* try hint first (convert 1-based to 0-based) */
	pos = hint > 0 ? hint - 1 : 0;
	if(trymatch(flines, nflines, pos, ctx, nctx)){
		free(ctx);
		free(op);
		return pos;
	}

	/* search outward from hint */
	for(i = 1; i <= Fuzzlines; i++){
		if(pos - i >= 0 && trymatch(flines, nflines, pos - i, ctx, nctx)){
			free(ctx);
			free(op);
			return pos - i;
		}
		if(pos + i <= nflines - nctx && trymatch(flines, nflines, pos + i, ctx, nctx)){
			free(ctx);
			free(op);
			return pos + i;
		}
	}

	free(ctx);
	free(op);
	return -1;
}

/*
 * Apply a single hunk at the given file position.  Produces a
 * new file lines array.  The old array is freed.
 *
 * Returns the new lines array and updates *nflinesp.  Also
 * updates *posp to point past the applied region in the new
 * file so that subsequent hunks search from the right place.
 */
static char**
applyhunk(char **flines, int *nflinesp, Hunk *h, int pos, int *posp)
{
	int nflines, nold, nnew, newcount, i;
	char **nfl;
	int cap;

	nflines = *nflinesp;

	/* count old lines (context + remove) and new lines (context + add) */
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

	newcount = nflines - nold + nnew;
	cap = newcount + 1;
	nfl = malloc(cap * sizeof(char*));
	if(nfl == nil)
		sysfatal("malloc: %r");

	/* copy lines before the hunk */
	for(i = 0; i < pos; i++)
		nfl[i] = strdup(flines[i]);

	/* apply the hunk */
	{
		int fi, ni, di;
		fi = pos;	/* file index (into old lines being consumed) */
		ni = pos;	/* new file index (into nfl being built) */

		for(di = 0; di < h->nlines; di++){
			switch(h->lines[di].op){
			case ' ':
				/* context: copy from old file (preserving original) */
				if(fi < nflines)
					nfl[ni] = strdup(flines[fi]);
				else
					nfl[ni] = strdup(h->lines[di].text);
				fi++;
				ni++;
				break;
			case '-':
				/* remove: skip old file line */
				fi++;
				break;
			case '+':
				/* add: insert new line */
				nfl[ni] = strdup(h->lines[di].text);
				ni++;
				break;
			}
		}

		/* copy remaining lines after the hunk */
		while(fi < nflines){
			nfl[ni] = strdup(flines[fi]);
			fi++;
			ni++;
		}

		*posp = ni - (nflines - fi);  /* should just be ni since fi==nflines */
		/* actually, posp should point just past the inserted region */
		*posp = pos + nnew;
	}

	/* free old lines */
	for(i = 0; i < nflines; i++)
		free(flines[i]);
	free(flines);

	*nflinesp = newcount;
	return nfl;
}

/*
 * Reassemble file lines into a single string.  Caller frees.
 */
static char*
joinlines(char **lines, int nlines)
{
	int i, total, off, len;
	char *buf;

	total = 0;
	for(i = 0; i < nlines; i++)
		total += strlen(lines[i]);

	buf = malloc(total + 1);
	if(buf == nil)
		sysfatal("malloc: %r");
	off = 0;
	for(i = 0; i < nlines; i++){
		len = strlen(lines[i]);
		memmove(buf + off, lines[i], len);
		off += len;
	}
	buf[off] = '\0';
	return buf;
}

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
	int fd;
	char *orig, *result;
	char **flines;
	int nflines;
	Patch *p;
	int i, pos, hint, applied;

	if(path == nil || path[0] == '\0')
		return smprint("error: no file path specified");
	if(diff == nil || diff[0] == '\0')
		return smprint("error: empty diff");

	/* parse the diff first so we fail fast on bad input */
	p = parsepatch(diff);
	if(p->nhunks == 0){
		freepatch(p);
		return smprint("error: no hunks found in diff");
	}

	/* read the original file; if it doesn't exist, start empty */
	fd = open(path, OREAD);
	if(fd < 0){
		/* file doesn't exist -- start with empty content */
		orig = strdup("");
	} else {
		orig = readfile(fd);
		close(fd);
		if(orig == nil){
			freepatch(p);
			return smprint("error: read %s: %r", path);
		}
	}

	flines = splitlines(orig, &nflines);
	free(orig);

	/* apply each hunk in order */
	applied = 0;
	hint = 0;
	for(i = 0; i < p->nhunks; i++){
		Hunk *h = &p->hunks[i];
		int starthint, nhunks;

		nhunks = p->nhunks;
		/* use the hunk's own line hint if available, else continue from last position */
		starthint = h->oldstart > 0 ? h->oldstart : hint + 1;
		pos = findhunk(flines, nflines, h, starthint);
		if(pos < 0){
			/* try harder: search from beginning */
			pos = findhunk(flines, nflines, h, 1);
		}
		if(pos < 0){
			freelines(flines, nflines);
			freepatch(p);
			return smprint("error: hunk %d/%d failed to match in %s",
				i + 1, nhunks, path);
		}
		flines = applyhunk(flines, &nflines, h, pos, &hint);
		applied++;
	}

	/* write the result */
	result = joinlines(flines, nflines);
	freelines(flines, nflines);
	freepatch(p);

	mkparents(path);
	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		free(result);
		return smprint("error: create %s: %r", path);
	}
	if(write(fd, result, strlen(result)) != (long)strlen(result)){
		close(fd);
		free(result);
		return smprint("error: write %s: %r", path);
	}
	close(fd);
	free(result);

	return smprint("patched %s (%d hunks)", path, applied);
}
