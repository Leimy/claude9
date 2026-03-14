#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

char *model = "claude-sonnet-4-20250514";
int maxtokens = 8192;
char *sysprompt = nil;

void
usage(void)
{
	fprint(2, "usage: %s [-m model] [-s sysprompt] [-t maxtokens]\n", argv0);
	exits("usage");
}

/*
 * Read multi-line input terminated by a line containing
 * only '.' (the Plan 9 convention, as in mail(1)).
 * Returns malloc'd string, or nil on EOF.
 */
char*
readinput(Biobuf *bin)
{
	char *line, *buf;
	int sz, n, len, first;

	sz = 1024;
	buf = malloc(sz);
	if(buf == nil)
		sysfatal("malloc: %r");
	n = 0;
	first = 1;

	for(;;){
		if(first)
			fprint(2, "> ");
		else
			fprint(2, "  ");
		line = Brdstr(bin, '\n', 1);
		if(line == nil){
			if(n > 0)
				break;
			free(buf);
			return nil;
		}
		len = strlen(line);

		/* '.' on its own line ends input */
		if(len == 1 && line[0] == '.'){
			free(line);
			break;
		}

		/* skip leading blank lines */
		if(first && len == 0){
			free(line);
			continue;
		}

		if(!first){
			/* add newline separator */
			if(n + 1 >= sz){
				sz *= 2;
				buf = realloc(buf, sz);
				if(buf == nil)
					sysfatal("realloc: %r");
			}
			buf[n++] = '\n';
		}

		if(n + len >= sz){
			while(n + len >= sz)
				sz *= 2;
			buf = realloc(buf, sz);
			if(buf == nil)
				sysfatal("realloc: %r");
		}
		memmove(buf + n, line, len);
		n += len;
		free(line);
		first = 0;
	}

	buf[n] = '\0';

	/* trim trailing whitespace */
	while(n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t' || buf[n-1] == '\n'))
		buf[--n] = '\0';

	if(n == 0){
		free(buf);
		return nil;
	}
	return buf;
}

void
main(int argc, char **argv)
{
	Conv *c;
	Biobuf bin;
	char *input, *reply, *apikey;

	ARGBEGIN{
	case 'm':
		model = EARGF(usage());
		break;
	case 's':
		sysprompt = EARGF(usage());
		break;
	case 't':
		maxtokens = atoi(EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND

	if(argc != 0)
		usage();

	apikey = getenv("ANTHROPIC_API_KEY");
	if(apikey == nil || apikey[0] == '\0'){
		fprint(2, "set $ANTHROPIC_API_KEY\n");
		exits("no api key");
	}

	c = convnew(apikey, model, maxtokens, sysprompt);
	Binit(&bin, 0, OREAD);

	fprint(2, "claude9 — model %s\n", model);
	fprint(2, "Enter message (end with '.' on its own line):\n\n");

	for(;;){
		input = readinput(&bin);
		if(input == nil)
			break;

		convappend(c, msgnew(Muser, input));
		fprint(2, "\n");

		reply = claudesend(c);
		if(reply == nil){
			fprint(2, "error: %r\n");
			free(input);
			continue;
		}

		convappend(c, msgnew(Massistant, reply));
		print("%s\n", reply);
		fprint(2, "\n");

		free(input);
		free(reply);
	}

	Bterm(&bin);
	convfree(c);
	exits(nil);
}
