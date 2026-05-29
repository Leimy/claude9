#include <u.h>
#include <libc.h>
#include "claude.h"

/*
 * Apply a unified diff by calling /bin/patch.
 *
 * The tool call provides the target file path separately from
 * the diff text.  /bin/patch extracts the path from the ---/+++
 * headers inside the diff, so we ensure those headers are
 * present and consistent with the path argument.
 *
 * Before invoking patch we rewrite the @@ hunk headers so that
 * the line counts are correct.  LLMs frequently get these wrong,
 * producing "oversized hunk" or "mismatched hunk size" errors
 * from patch(1).  Since the counts are redundant (they can be
 * derived by counting the -, +, and context lines in the hunk
 * body), we just recompute them.
 *
 * The diff is written to a temporary file and passed to:
 *   patch -p0 -d / <tmpfile
 *
 * Parent directories are created before patching so that
 * new-file diffs (--- /dev/null) work.
 */

/*
 * Scan the diff text for a --- line.  Returns 1 if found.
 */
static int
hashdr(char *diff)
{
	char *p;

	for(p = diff; *p != '\0'; ){
		if(strncmp(p, "--- ", 4) == 0)
			return 1;
		p = strchr(p, '\n');
		if(p == nil)
			break;
		p++;
	}
	return 0;
}

/*
 * Count the number of @@ hunk headers in the diff.
 */
static int
counthunks(char *diff)
{
	char *p;
	int n;

	n = 0;
	for(p = diff; *p != '\0'; ){
		if(strncmp(p, "@@", 2) == 0)
			n++;
		p = strchr(p, '\n');
		if(p == nil)
			break;
		p++;
	}
	return n;
}

/*
 * Check whether the file at path exists.
 */
static int
fileexists(char *path)
{
	Dir *d;

	d = dirstat(path);
	if(d == nil)
		return 0;
	free(d);
	return 1;
}

/*
 * Build a complete diff string with proper --- / +++ headers.
 * If the diff already has headers, return a strdup of it.
 * If the target file doesn't exist, use /dev/null as the old file.
 * Caller frees the result.
 */
static char*
ensurehdrs(char *path, char *diff)
{
	char *old;

	if(hashdr(diff))
		return estrdup(diff);

	if(fileexists(path))
		old = path;
	else
		old = "/dev/null";

	return esmprint("--- %s\n+++ %s\n%s", old, path, diff);
}

/*
 * Rewrite @@ hunk headers so line counts match reality.
 *
 * For each hunk we count the actual old lines (context + removed)
 * and new lines (context + added), then emit a corrected header.
 * The starting line numbers from the original header are preserved
 * as-is since patch(1) uses them only as hints.
 *
 * Lines that don't start with ' ', '-', '+', or '@@' inside a
 * hunk body are treated as context (they're probably missing the
 * leading space -- an LLM mistake we tolerate by prepending one).
 *
 * Returns a malloc'd corrected diff.  Caller frees.
 */
static char*
fixhunks(char *diff)
{
	char *out, *p, *linestart, *eol;
	int outcap, outlen;
	int oldstart, newstart;
	int inbody;
	char *body;
	int bodycap, bodylen;
	int nold, nnew;
	char hdr[128];
	int hlen;

	outcap = strlen(diff) * 2 + 256;
	out = emalloc(outcap);
	outlen = 0;

	bodycap = 4096;
	body = emalloc(bodycap);
	bodylen = 0;

	inbody = 0;
	oldstart = 1;
	newstart = 1;
	nold = 0;
	nnew = 0;

	for(p = diff; *p != '\0'; ){
		linestart = p;
		eol = strchr(p, '\n');
		if(eol != nil)
			eol++;	/* include the \n */
		else
			eol = p + strlen(p);

		/* Check for @@ header */
		if(strncmp(linestart, "@@", 2) == 0){
			/* flush previous hunk if any */
			if(inbody){
				hlen = snprint(hdr, sizeof hdr,
					"@@ -%d,%d +%d,%d @@\n",
					oldstart, nold, newstart, nnew);
				while(outlen + hlen + bodylen + 1 > outcap){
					outcap *= 2;
					out = erealloc(out, outcap);
				}
				memmove(out + outlen, hdr, hlen);
				outlen += hlen;
				memmove(out + outlen, body, bodylen);
				outlen += bodylen;
				bodylen = 0;
				nold = 0;
				nnew = 0;
			}
			inbody = 1;
			/*
			 * Parse starting line numbers from the original
			 * header.  Format: @@ -OLD,OLDCNT +NEW,NEWCNT @@
			 * We only need OLD and NEW start numbers.
			 */
			{
				char *q;
				q = linestart + 2;
				while(*q == ' ') q++;
				if(*q == '-') q++;
				oldstart = atoi(q);
				if(oldstart < 1) oldstart = 1;
				q = strchr(q, '+');
				if(q != nil){
					q++;
					newstart = atoi(q);
					if(newstart < 1) newstart = 1;
				}
			}
			p = eol;
			continue;
		}

		/* --- and +++ headers go straight to output */
		if(strncmp(linestart, "--- ", 4) == 0
		|| strncmp(linestart, "+++ ", 4) == 0){
			/* flush previous hunk if any */
			if(inbody){
				hlen = snprint(hdr, sizeof hdr,
					"@@ -%d,%d +%d,%d @@\n",
					oldstart, nold, newstart, nnew);
				while(outlen + hlen + bodylen + 1 > outcap){
					outcap *= 2;
					out = erealloc(out, outcap);
				}
				memmove(out + outlen, hdr, hlen);
				outlen += hlen;
				memmove(out + outlen, body, bodylen);
				outlen += bodylen;
				bodylen = 0;
				nold = 0;
				nnew = 0;
				inbody = 0;
			}
			while(outlen + (int)(eol - linestart) + 1 > outcap){
				outcap *= 2;
				out = erealloc(out, outcap);
			}
			memmove(out + outlen, linestart, eol - linestart);
			outlen += eol - linestart;
			p = eol;
			continue;
		}

		if(!inbody){
			/* Before first hunk -- pass through */
			while(outlen + (int)(eol - linestart) + 1 > outcap){
				outcap *= 2;
				out = erealloc(out, outcap);
			}
			memmove(out + outlen, linestart, eol - linestart);
			outlen += eol - linestart;
			p = eol;
			continue;
		}

		/* Inside a hunk body */
		if(linestart[0] == ' '){
			nold++;
			nnew++;
			while(bodylen + (int)(eol - linestart) + 1 > bodycap){
				bodycap *= 2;
				body = erealloc(body, bodycap);
			}
			memmove(body + bodylen, linestart, eol - linestart);
			bodylen += eol - linestart;
		} else if(linestart[0] == '-'){
			nold++;
			while(bodylen + (int)(eol - linestart) + 1 > bodycap){
				bodycap *= 2;
				body = erealloc(body, bodycap);
			}
			memmove(body + bodylen, linestart, eol - linestart);
			bodylen += eol - linestart;
		} else if(linestart[0] == '+'){
			nnew++;
			while(bodylen + (int)(eol - linestart) + 1 > bodycap){
				bodycap *= 2;
				body = erealloc(body, bodycap);
			}
			memmove(body + bodylen, linestart, eol - linestart);
			bodylen += eol - linestart;
		} else if(linestart[0] == '\\'){
			/* "\ No newline at end of file" -- pass through */
			while(bodylen + (int)(eol - linestart) + 1 > bodycap){
				bodycap *= 2;
				body = erealloc(body, bodycap);
			}
			memmove(body + bodylen, linestart, eol - linestart);
			bodylen += eol - linestart;
		} else {
			/*
			 * Line without a diff prefix inside a hunk.
			 * This is almost certainly a context line that
			 * the LLM forgot to prefix with a space.
			 * Treat it as context.
			 */
			nold++;
			nnew++;
			while(bodylen + 1 + (int)(eol - linestart) + 1 > bodycap){
				bodycap *= 2;
				body = erealloc(body, bodycap);
			}
			body[bodylen++] = ' ';
			memmove(body + bodylen, linestart, eol - linestart);
			bodylen += eol - linestart;
		}
		p = eol;
	}

	/* flush last hunk */
	if(inbody){
		hlen = snprint(hdr, sizeof hdr,
			"@@ -%d,%d +%d,%d @@\n",
			oldstart, nold, newstart, nnew);
		while(outlen + hlen + bodylen + 1 > outcap){
			outcap *= 2;
			out = erealloc(out, outcap);
		}
		memmove(out + outlen, hdr, hlen);
		outlen += hlen;
		memmove(out + outlen, body, bodylen);
		outlen += bodylen;
	}

	out[outlen] = '\0';
	free(body);
	return out;
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
	char *fulldiff, *fixeddiff, *data, *result;
	int pfd[2];		/* patch stdout+stderr */
	int dfd[2];		/* diff fed to patch stdin */
	int nhunks;
	long n, len;

	if(path == nil || path[0] == '\0')
		return esmprint("error: no file path specified");
	if(diff == nil || diff[0] == '\0')
		return esmprint("error: empty diff");

	nhunks = counthunks(diff);
	if(nhunks == 0)
		return esmprint("error: no hunks found in diff");

	mkparents(path);

	fulldiff = ensurehdrs(path, diff);
	fixeddiff = fixhunks(fulldiff);
	free(fulldiff);

	if(pipe(pfd) < 0){
		free(fixeddiff);
		return esmprint("error: pipe: %r");
	}
	if(pipe(dfd) < 0){
		close(pfd[0]);
		close(pfd[1]);
		free(fixeddiff);
		return esmprint("error: pipe: %r");
	}

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		close(dfd[0]);
		close(dfd[1]);
		free(fixeddiff);
		return esmprint("error: fork: %r");
	case 0:
		close(pfd[0]);
		close(dfd[1]);
		dup(dfd[0], 0);
		close(dfd[0]);
		dup(pfd[1], 1);
		dup(pfd[1], 2);
		close(pfd[1]);
		execl("/bin/patch", "patch", "-p0", "-d", "/", nil);
		fprint(2, "exec patch: %r\n");
		exits("exec");
	}

	close(pfd[1]);
	close(dfd[0]);

	/* write the diff to patch's stdin */
	n = strlen(fixeddiff);
	if(write(dfd[1], fixeddiff, n) != n){
		close(dfd[1]);
		close(pfd[0]);
		free(fixeddiff);
		/* still need to reap child */
		waitpid();
		return esmprint("error: write to patch: %r");
	}
	close(dfd[1]);
	free(fixeddiff);

	/* read patch's output */
	data = emalloc(8192);
	len = 0;
	while((n = read(pfd[0], data + len, 8192)) > 0){
		len += n;
		data = erealloc(data, len + 8192);
	}
	close(pfd[0]);
	data[len] = '\0';

	/* reap child and check exit status */
	{
		char *tmp;
		Waitmsg *w;

		tmp = nil;
		w = wait();
		if(w != nil){
			if(w->msg[0] != '\0')
				tmp = estrdup(w->msg);
			free(w);
		}

		if(tmp != nil){
			/* patch failed */
			while(len > 0 && (data[len-1] == '\n' || data[len-1] == ' '))
				data[--len] = '\0';
			if(len > 0)
				result = esmprint("error: patch %s failed: %s\n%s",
					path, tmp, data);
			else
				result = esmprint("error: patch %s failed: %s",
					path, tmp);
			free(tmp);
			free(data);
			return result;
		}
	}

	free(data);
	return esmprint("patched %s (%d hunks)", path, nhunks);
}
