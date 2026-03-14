#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"


Action *lastactions;
static char *fileshdr = "---files";

ModelInfo *models;
int nmodels;

char *apikey;
char *model = "claude-opus-4-6";
int maxtokens = 65536;
char *sysprompt = nil;

/*
 * Progress indicator: a background process that prints
 * dots to stderr while waiting for the API response.
 * We fork a child that prints a dot every 2 seconds.
 * The parent kills it when the response arrives.
 */
static int progresspid = -1;

static void
startprogress(int nmsg, long nbytes)
{
	int pid;
	int elapsed;

	fprint(2, "sending %d messages (%ld bytes)...", nmsg, nbytes);
	pid = rfork(RFPROC|RFMEM);
	if(pid < 0){
		/* fork failed, no progress indicator */
		fprint(2, "\n");
		return;
	}
	if(pid > 0){
		/* parent */
		progresspid = pid;
		return;
	}
	/* child: print dots */
	elapsed = 0;
	for(;;){
		sleep(2000);
		elapsed += 2;
		fprint(2, " %ds", elapsed);
	}
}

static void
stopprogress(void)
{
	if(progresspid > 0){
		postnote(PNPROC, progresspid, "kill");
		waitpid();
		progresspid = -1;
	}
	fprint(2, "\n");
}

static void
showusage(Usage *u)
{
	if(u->input_tokens > 0 || u->output_tokens > 0){
		fprint(2, "[tokens: %d in, %d out, %d total",
			u->input_tokens, u->output_tokens,
			u->input_tokens + u->output_tokens);
		if(u->stop_reason != nil)
			fprint(2, ", stop: %s", u->stop_reason);
		fprint(2, "]\n");
	}
}


static char *families[] = {
	"opus", "sonnet", "haiku",
};

static int
isdate(char *s)
{
	int i;

	for(i = 0; i < 8; i++)
		if(s[i] < '0' || s[i] > '9')
			return 0;
	return s[8] == '\0' || s[8] == '-';
}

char*
idalias(char *id)
{
	static char buf[64];
	char *p, *fam, *s;
	int i, major, minor, hasmajor, hasminor;

	fam = nil;
	for(i = 0; i < nelem(families); i++){
		p = strstr(id, families[i]);
		if(p != nil){
			fam = families[i];
			break;
		}
	}
	if(fam == nil)
		return id;

	hasmajor = 0;
	hasminor = 0;
	major = 0;
	minor = 0;

	s = id;
	if(strncmp(s, "claude-", 7) == 0)
		s += 7;

	while(*s != '\0'){
		if(*s >= '0' && *s <= '9' && !isdate(s)){
			int v = 0;
			char *t = s;
			while(*t >= '0' && *t <= '9')
				v = v * 10 + (*t++ - '0');
			if(*t == '\0' || *t == '-'){
				if(!hasmajor){
					major = v;
					hasmajor = 1;
				} else if(!hasminor){
					minor = v;
					hasminor = 1;
				}
			}
		}
		p = strchr(s, '-');
		if(p == nil)
			break;
		s = p + 1;
	}

	if(hasmajor && hasminor)
		snprint(buf, sizeof buf, "%s%d.%d", fam, major, minor);
	else if(hasmajor)
		snprint(buf, sizeof buf, "%s%d", fam, major);
	else
		snprint(buf, sizeof buf, "%s", fam);
	return buf;
}

void
freemodels(void)
{
	int i;

	for(i = 0; i < nmodels; i++){
		free(models[i].id);
		free(models[i].display_name);
	}
	free(models);
	models = nil;
	nmodels = 0;
}

int
loadmodels(void)
{
	ModelInfo *list;
	int n;

	n = fetchmodels(apikey, &list);
	if(n < 0){
		fprint(2, "error fetching models: %r\n");
		return 0;
	}
	freemodels();
	models = list;
	nmodels = n;
	return 1;
}

void
pusage(void)
{
	fprint(2, "usage: %s [-m model] [-s sysprompt] [-t maxtokens]\n", argv0);
	exits("usage");
}

ModelInfo*
findmodel(char *name)
{
	int i;

	for(i = 0; i < nmodels; i++){
		if(models[i].id != nil && cistrcmp(name, models[i].id) == 0)
			return &models[i];
		if(models[i].id != nil && cistrcmp(name, idalias(models[i].id)) == 0)
			return &models[i];
		if(models[i].display_name != nil && cistrcmp(name, models[i].display_name) == 0)
			return &models[i];
	}
	return nil;
}

char*
modelalias(char *id)
{
	return idalias(id);
}

void
cmdread(Conv *c, char *arg)
{
	char *p, *comment, *contents, *reply;
	int fd;
	Usage usage;
	int nmsg;
	char *paths[64];
	int npaths, i;
	int msglen, msgcap;
	char *msg;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "usage: /read <file> [file ...] [-- comment]\n");
		return;
	}

	comment = nil;
	npaths = 0;
	p = arg;
	while(*p != '\0' && npaths < nelem(paths)){
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p == '\0')
			break;
		if(p[0] == '-' && p[1] == '-' && (p[2] == ' ' || p[2] == '\t' || p[2] == '\0')){
			p += 2;
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p != '\0')
				comment = p;
			break;
		}
		paths[npaths++] = p;
		while(*p != '\0' && *p != ' ' && *p != '\t')
			p++;
		if(*p != '\0')
			*p++ = '\0';
	}

	if(npaths == 0){
		fprint(2, "no files specified\n");
		return;
	}

	msgcap = 4096;
	msglen = 0;
	msg = malloc(msgcap);
	if(msg == nil){
		fprint(2, "malloc: %r\n");
		return;
	}
	msg[0] = '\0';

	for(i = 0; i < npaths; i++){
		fd = open(paths[i], OREAD);
		if(fd < 0){
			fprint(2, "error opening %s: %r\n", paths[i]);
			free(msg);
			return;
		}
		contents = readfile(fd);
		close(fd);
		if(contents == nil){
			fprint(2, "error reading %s: %r\n", paths[i]);
			free(msg);
			return;
		}
		{
		int clen, plen, need;
		clen = strlen(contents);
		plen = strlen(paths[i]);
		need = msglen + 5 + plen + 5 + clen + 2;
		if(need > msgcap){
			while(need > msgcap)
				msgcap *= 2;
			msg = realloc(msg, msgcap);
			if(msg == nil){
				free(contents);
				sysfatal("realloc: %r");
			}
		}
		msglen += snprint(msg + msglen, msgcap - msglen,
			"--- %s ---\n%s\n", paths[i], contents);
		free(contents);
		}
		fprint(2, "  added %s\n", paths[i]);
	}

	convappend(c, msgnew(Muser, msg));
	if(comment != nil && comment[0] != '\0')
		convappend(c, msgnew(Muser, comment));

	nmsg = convcount(c);
	startprogress(nmsg, convsize(c));
	memset(&usage, 0, sizeof usage);
	reply = claudesend(c, &usage);
	stopprogress();
	showusage(&usage);

	fprint(2, "...\n");
	if(reply == nil){
		fprint(2, "error: %r\n");
		free(msg);
		free(usage.stop_reason);
		return;
	}

	convappend(c, msgnew(Massistant, reply));
	print("%s\n", reply);
	fprint(2, "\n");

	free(msg);
	free(reply);
	free(usage.stop_reason);
}

void
cmdhelp(void)
{
	fprint(2, "Commands:\n");
	fprint(2, "  /models          list available models (fetched from API)\n");
	fprint(2, "  /model           show current model\n");
	fprint(2, "  /model <name>    switch to model (alias or full id)\n");
	fprint(2, "  /read <file> [file ...] [-- comment]  read files\n");
	fprint(2, "  /save <file>     save conversation to file\n");
	fprint(2, "  /load <file>     load conversation from file\n");
	fprint(2, "  /beadsave <file> save condensed bead (summary + file refs)\n");
	fprint(2, "  /beadload <file> load bead, re-reading files from disk\n");
	fprint(2, "  /clear           clear conversation history\n");
	fprint(2, "  /tokens          show current max tokens\n");
	fprint(2, "  /tokens <n>      set max tokens\n");
	fprint(2, "  /help            show this help\n");
	fprint(2, "  /apply [all|N]   apply file actions from last reply\n");
	fprint(2, "\n");
}

void
cmdtokens(Conv *c, char *arg)
{
	int n;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "max tokens: %d\n", c->maxtokens);
		return;
	}
	n = atoi(arg);
	if(n <= 0){
		fprint(2, "invalid token count: %s\n", arg);
		return;
	}
	c->maxtokens = n;
	fprint(2, "max tokens set to %d\n", c->maxtokens);
}

void
cmdclear(Conv *c)
{
	Msg *m, *next;
	int n;

	n = 0;
	for(m = c->msgs; m != nil; m = next){
		next = m->next;
		free(m->text);
		free(m);
		n++;
	}
	c->msgs = nil;
	c->tail = nil;
	freeactions(lastactions);
	lastactions = nil;
	fprint(2, "cleared %d messages\n", n);
}

static char *msgsep = "---msg";
static char *modelhdr = "---model";
static char *maxtokenshdr = "---maxtokens";
static char *sysprompthdr = "---sysprompt";

static void
writemsgbody(int fd, char *text)
{
	char *sol, *eol;

	for(sol = text; *sol != '\0'; sol = eol){
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
	if(text[0] != '\0' && text[strlen(text)-1] != '\n')
		fprint(fd, "\n");
}

void
cmdsave(Conv *c, char *arg)
{
	Msg *m;
	int fd;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "usage: /save <file>\n");
		return;
	}

	mkparents(arg);
	fd = create(arg, OWRITE, 0666);
	if(fd < 0){
		fprint(2, "save: %r\n");
		return;
	}

	fprint(fd, "%s %s\n", modelhdr, c->model);
	fprint(fd, "%s %d\n", maxtokenshdr, c->maxtokens);
	if(c->sysprompt != nil){
		fprint(fd, "%s\n", sysprompthdr);
		fprint(fd, "%s\n", c->sysprompt);
	}

	for(m = c->msgs; m != nil; m = m->next){
		fprint(fd, "%s %s\n", msgsep,
			m->role == Muser ? "user" : "assistant");
		writemsgbody(fd, m->text);
	}

	close(fd);
	fprint(2, "saved to %s\n", arg);
}

void
collectfiles(Conv *c, char *names, int namesz)
{
	Msg *m;
	char tmp[512];
	char *s, *e, *nl, *found;
	int nlen;
	int tlen, dup;

	nlen = 0;
	names[0] = '\0';
	for(m = c->msgs; m != nil; m = m->next){
		if(m->role != Muser)
			continue;
		s = m->text;
		for(;;){
			/* find next file header: --- name --- */
			while(*s != '\0' && strncmp(s, "--- ", 4) != 0){
				nl = strchr(s, '\n');
				if(nl == nil)
					break;
				s = nl + 1;
			}
			if(strncmp(s, "--- ", 4) != 0)
				break;
			s += 4;
			e = strstr(s, " ---");
			if(e == nil)
				break;
			tlen = e - s;
			if(tlen >= (int)sizeof tmp)
				tlen = (int)sizeof tmp - 1;
			memmove(tmp, s, tlen);
			tmp[tlen] = '\0';
			/* check if already recorded (exact word match) */
			dup = 0;
			found = names;
			while((found = strstr(found, tmp)) != nil){
				if((found == names || found[-1] == ' ') &&
				   (found[tlen] == '\0' || found[tlen] == ' ')){
					dup = 1;
					break;
				}
				found++;
			}
			if(!dup && tlen > 0){
				if(nlen > 0 && nlen + 1 < namesz)
					names[nlen++] = ' ';
				if(nlen + tlen < namesz){
					memmove(names + nlen, s, tlen);
					nlen += tlen;
					names[nlen] = '\0';
				}
			}
			/* skip past " ---" and advance to next line */
			s = e + 4;
			nl = strchr(s, '\n');
			if(nl == nil)
				break;
			s = nl + 1;
		}
	}
}

void
beadsave(Conv *c, char *arg)
{
	Msg *m;
	int fd;
	char names[4096];
	char *summary, *prompt;
	Conv *tmp;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "usage: /beadsave <file>\n");
		return;
	}

	/* ask Claude to summarize the conversation */
	fprint(2, "generating summary...\n");
	tmp = convnew(c->apikey, c->model, c->maxtokens, nil);
	fprint(2, "  copying %d messages...\n", convcount(c));
	for(m = c->msgs; m != nil; m = m->next)
		convappend(tmp, msgnew(m->role, m->text));
	prompt = "Summarize this conversation concisely for future context. "
		"Cover: what files are involved, what was discussed, what changes "
		"were made or planned, and the current state. Be brief but complete. "
		"This will be used to resume the conversation later.";
	convappend(tmp, msgnew(Muser, prompt));
	fprint(2, "  sending summary request (%ld bytes)...\n", convsize(tmp));
	summary = claudesend(tmp, nil);
	convfree(tmp);
	if(summary == nil){
		fprint(2, "  claudesend returned nil\n");
		fprint(2, "beadsave: summary failed: %r\n");
		return;
	}

	/* collect all file names from the conversation */
	collectfiles(c, names, sizeof names);
	fprint(2, "  files: %s\n", names[0] ? names : "(none)");

	mkparents(arg);
	fd = create(arg, OWRITE, 0666);
	if(fd < 0){
		free(summary);
		fprint(2, "beadsave: %r\n");
		return;
	}

	fprint(fd, "%s %s\n", modelhdr, c->model);
	fprint(fd, "%s %d\n", maxtokenshdr, c->maxtokens);
	if(c->sysprompt != nil){
		fprint(fd, "%s\n", sysprompthdr);
		fprint(fd, "%s\n", c->sysprompt);
	}

	fprint(fd, "%s assistant\n", msgsep);
	writemsgbody(fd, summary);
	if(names[0] != '\0'){
		fprint(fd, "%s user\n", msgsep);
		fprint(fd, "%s %s\n", fileshdr, names);
	}

	close(fd);
	fprint(2, "bead saved to %s\n", arg);
	free(summary);
}

void
cmdload(Conv *c, char *arg)
{
	int fd;
	char *data, *p, *line, *eol;
	Msg *m, *next;
	char *v, *r;
	int llen, need;
	int msglen, msgcap;
	char *msgbuf;
	int role;
	int nmsg;
	enum { Shdr, Ssysprompt, Smsg } state;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "usage: /load <file>\n");
		return;
	}

	fd = open(arg, OREAD);
	if(fd < 0){
		fprint(2, "load: %r\n");
		return;
	}
	data = readfile(fd);
	close(fd);
	if(data == nil){
		fprint(2, "load: read: %r\n");
		return;
	}

	for(m = c->msgs; m != nil; m = next){
		next = m->next;
		free(m->text);
		free(m);
	}
	c->msgs = nil;
	c->tail = nil;

	state = Shdr;
	role = Muser;
	msgbuf = nil;
	msglen = 0;
	msgcap = 0;
	nmsg = 0;
	p = data;

	while(*p != '\0'){
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
				convappend(c, msgnew(role, msgbuf));
				nmsg++;
			}
			if(state == Ssysprompt && msgbuf != nil){
				while(msglen > 0 && msgbuf[msglen-1] == '\n')
					msglen--;
				msgbuf[msglen] = '\0';
				free(c->sysprompt);
				c->sysprompt = strdup(msgbuf);
			}
			r = line + strlen(msgsep);
			while(*r == ' ') r++;
			if(strcmp(r, "assistant") == 0)
				role = Massistant;
			else
				role = Muser;
			state = Smsg;
			msglen = 0;
			continue;
		}

		if(state == Shdr){
			if(strncmp(line, modelhdr, strlen(modelhdr)) == 0){
				v = line + strlen(modelhdr);
				while(*v == ' ') v++;
				if(*v != '\0'){
					free(c->model);
					c->model = strdup(v);
				}
			} else if(strncmp(line, maxtokenshdr, strlen(maxtokenshdr)) == 0){
				v = line + strlen(maxtokenshdr);
				while(*v == ' ') v++;
				if(*v != '\0')
					c->maxtokens = atoi(v);
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
		convappend(c, msgnew(role, msgbuf));
		nmsg++;
	}
	if(state == Ssysprompt && msgbuf != nil && msglen > 0){
		while(msglen > 0 && msgbuf[msglen-1] == '\n')
			msglen--;
		msgbuf[msglen] = '\0';
		free(c->sysprompt);
		c->sysprompt = strdup(msgbuf);
	}

	free(msgbuf);
	free(data);
	fprint(2, "loaded %s: %d messages, model %s\n", arg, nmsg, modelalias(c->model));
}

void
beadload(Conv *c, char *arg)
{
	Msg *m;
	int fd;

	cmdload(c, arg);
	if(c->msgs == nil)
		return;

	for(m = c->msgs; m != nil; m = m->next){
		char *p, *files;
		int msglen, msgcap;
		char *msg;

		if(m->role != Muser)
			continue;
		if(strncmp(m->text, fileshdr, strlen(fileshdr)) != 0)
			continue;

		files = m->text + strlen(fileshdr);
		while(*files == ' ')
			files++;

		msgcap = 4096;
		msglen = 0;
		msg = malloc(msgcap);
		if(msg == nil)
			sysfatal("malloc: %r");
		msg[0] = '\0';

		p = files;
		while(*p != '\0'){
			char *name, *contents;
			int clen, nlen, need;

			while(*p == ' ')
				p++;
			if(*p == '\0')
				break;
			name = p;
			while(*p != '\0' && *p != ' ')
				p++;
			if(*p != '\0')
				*p++ = '\0';

			fd = open(name, OREAD);
			if(fd < 0){
				fprint(2, "beadload: %s: %r\n", name);
				continue;
			}
			contents = readfile(fd);
			close(fd);
			if(contents == nil){
				fprint(2, "beadload: read %s: %r\n", name);
				continue;
			}

			clen = strlen(contents);
			nlen = strlen(name);
			need = msglen + 5 + nlen + 5 + clen + 2;
			if(need > msgcap){
				while(need > msgcap)
					msgcap *= 2;
				msg = realloc(msg, msgcap);
				if(msg == nil)
					sysfatal("realloc: %r");
			}
			msglen += snprint(msg + msglen, msgcap - msglen,
				"--- %s ---\n%s\n", name, contents);
			free(contents);
			fprint(2, "  loaded %s\n", name);
		}

		if(msglen > 0){
			free(m->text);
			m->text = msg;
		} else
			free(msg);
	}

	/* print summary so user knows what was loaded */
	if(c->msgs != nil && c->msgs->role == Massistant)
		fprint(2, "\ncontext summary:\n%s\n\n", c->msgs->text);
}

void
cmdmodels(char *current)
{
	int i;
	char mark;
	char *alias, *name;

	fprint(2, "fetching models...\n");
	loadmodels();

	if(nmodels == 0){
		fprint(2, "no models available\n\n");
		return;
	}

	fprint(2, "  %-10s %-30s  %s\n", "ALIAS", "MODEL ID", "DISPLAY NAME");
	for(i = 0; i < nmodels; i++){
		if(models[i].id == nil)
			continue;
		mark = (strcmp(current, models[i].id) == 0) ? '*' : ' ';
		alias = idalias(models[i].id);
		name = models[i].display_name;
		if(name == nil)
			name = "";
		fprint(2, "%c %-10s %-30s  %s\n",
			mark, alias, models[i].id, name);
	}
	fprint(2, "\n  * = active\n\n");
}

void
cmdmodel(Conv *c, char *arg)
{
	ModelInfo *m;

	if(arg == nil || arg[0] == '\0'){
		fprint(2, "model: %s (%s)\n", modelalias(c->model), c->model);
		return;
	}
	if(nmodels == 0)
		loadmodels();
	m = findmodel(arg);
	if(m == nil){
		fprint(2, "unknown model: %s\n", arg);
		fprint(2, "use /models to see available models\n");
		return;
	}
	free(c->model);
	c->model = strdup(m->id);
	if(c->model == nil)
		sysfatal("strdup: %r");
	c->maxtokens = m->max_output_tokens;
	fprint(2, "switched to %s (%s)\n", idalias(m->id), m->id);
	if(m->display_name != nil)
		fprint(2, "  %s\n", m->display_name);
	fprint(2, "  max_tokens: %d\n", c->maxtokens);
	fprint(2, "\n");
}

void
cmdapply(char *arg)
{
	Action *a;
	int n, sel;

	if(lastactions == nil){
		fprint(2, "no pending actions\n");
		return;
	}
	if(arg == nil || strcmp(arg, "all") == 0){
		for(a = lastactions; a != nil; a = a->next)
			applyaction(a);
		return;
	}
	sel = atoi(arg);
	if(sel <= 0){
		fprint(2, "usage: /apply [all | N]\n");
		return;
	}
	n = 1;
	for(a = lastactions; a != nil; a = a->next){
		if(n == sel){
			applyaction(a);
			return;
		}
		n++;
	}
	fprint(2, "no action [%d]\n", sel);
}

int
handlecmd(Conv *c, char *input)
{
	char *arg;

	if(input[0] != '/')
		return 0;
	input++;
	arg = strchr(input, ' ');
	if(arg != nil){
		*arg++ = '\0';
		while(*arg == ' ' || *arg == '\t')
			arg++;
		if(*arg == '\0')
			arg = nil;
	}
	if(strcmp(input, "help") == 0 || strcmp(input, "?") == 0){
		cmdhelp();
		return 1;
	}
	if(strcmp(input, "models") == 0){
		cmdmodels(c->model);
		return 1;
	}
	if(strcmp(input, "model") == 0){
		cmdmodel(c, arg);
		return 1;
	}
	if(strcmp(input, "tokens") == 0){
		cmdtokens(c, arg);
		return 1;
	}
	if(strcmp(input, "clear") == 0){
		cmdclear(c);
		return 1;
	}
	if(strcmp(input, "read") == 0){
		cmdread(c, arg);
		return 1;
	}
	if(strcmp(input, "apply") == 0){
		cmdapply(arg);
		return 1;
	}
	if(strcmp(input, "save") == 0){
		cmdsave(c, arg);
		return 1;
	}
	if(strcmp(input, "load") == 0){
		cmdload(c, arg);
		return 1;
	}
	if(strcmp(input, "beadsave") == 0){
		beadsave(c, arg);
		return 1;
	}
	if(strcmp(input, "beadload") == 0){
		beadload(c, arg);
		return 1;
	}
	fprint(2, "unknown command: /%s\n", input);
	fprint(2, "type /help for available commands\n\n");
	return 1;
}

char*
readinput(Biobuf *bin, char *curmodel, int tokens)
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
			fprint(2, "%s/%d> ", modelalias(curmodel), tokens);
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
		while(len > 0 && line[len-1] == '\r')
			line[--len] = '\0';
		if(first && len > 0 && line[0] == '/'){
			free(buf);
			return line;
		}
		if(len == 1 && line[0] == '.'){
			free(line);
			break;
		}
		if(first && len == 0){
			free(line);
			continue;
		}
		if(!first){
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
	char *input, *reply;
	Usage usage;
	int nmsg;

	ARGBEGIN{
	case 'm':
		model = EARGF(pusage());
		break;
	case 's':
		sysprompt = EARGF(pusage());
		break;
	case 't':
		maxtokens = atoi(EARGF(pusage()));
		break;
	default:
		pusage();
	}ARGEND

	if(argc != 0)
		pusage();

	apikey = getenv("ANTHROPIC_API_KEY");
	if(apikey == nil || apikey[0] == '\0'){
		fprint(2, "set $ANTHROPIC_API_KEY\n");
		exits("no api key");
	}

	c = convnew(apikey, model, maxtokens, sysprompt);
	Binit(&bin, 0, OREAD);

	fprint(2, "claude9 - %s (%s)\n", modelalias(c->model), c->model);
	fprint(2, "type /help for commands, end messages with '.' on its own line\n\n");

	for(;;){
		input = readinput(&bin, c->model, c->maxtokens);
		if(input == nil)
			break;
		if(handlecmd(c, input)){
			free(input);
			continue;
		}
		convappend(c, msgnew(Muser, input));
		fprint(2, "\n");
		fprint(2, "...\n");

		nmsg = convcount(c);
		startprogress(nmsg, convsize(c));
		reply = claudesend(c, &usage);
		stopprogress();
		if(reply == nil){
			fprint(2, "error: %r\n");
			free(input);
			continue;
		}
		convappend(c, msgnew(Massistant, reply));

		freeactions(lastactions);
		showusage(&usage);
		lastactions = parseactions(reply);
		if(lastactions != nil){
			char *prose;
			Action *a;
			int n;

			prose = stripactions(reply);
			if(prose[0] != '\0')
				print("%s\n", prose);
			free(prose);

			fprint(2, "\nactions:\n");
			n = 1;
			for(a = lastactions; a != nil; a = a->next)
				showaction(a, n++);
			fprint(2, "use /apply to apply, /apply N for selective\n");
		} else {
			print("%s\n", reply);
			if(strstr(reply, "<<<ACTION") != nil)
				fprint(2, "[debug: <<<ACTION marker found but "
					"parseactions returned nil -- "
					"possible non-ASCII chars or "
					"malformed block]\n");
		}
		fprint(2, "\n");

		free(input);
		free(reply);
	}

	Bterm(&bin);
	convfree(c);
	exits(nil);
}
