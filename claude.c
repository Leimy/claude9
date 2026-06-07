#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

static char *apiurl = "https://api.anthropic.com/v1/messages";
static char *modelsurl = "https://api.anthropic.com/v1/models?limit=100";
static char *apiversion = "2023-06-01";

/*
 * emalloc wrappers: succeed or sysfatal.
 */
void*
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc %lud: %r", n);
	return p;
}

void*
erealloc(void *v, ulong n)
{
	void *p;

	p = realloc(v, n);
	if(p == nil)
		sysfatal("realloc %lud: %r", n);
	return p;
}

void*
emallocz(ulong n, int clr)
{
	void *p;

	p = mallocz(n, clr);
	if(p == nil)
		sysfatal("mallocz %lud: %r", n);
	return p;
}

char*
estrdup(char *s)
{
	char *p;

	p = strdup(s);
	if(p == nil)
		sysfatal("strdup: %r");
	return p;
}

char*
esmprint(char *fmt, ...)
{
	char *p;
	va_list arg;

	va_start(arg, fmt);
	p = vsmprint(fmt, arg);
	va_end(arg);
	if(p == nil)
		sysfatal("smprint: %r");
	return p;
}

/*
 * Single source of truth for the tools we expose to Claude.
 *
 * Every tool takes a "path" parameter; tools that also need a
 * second string parameter declare it in the bodyparam / bodydesc
 * fields.  edit_file is special: it has integer start/end params
 * and a replacement string, handled via custom code in mktools()
 * and parseinput().  findtool() / findtooltype() let the rest of
 * the code walk between API names and the Acreate/Aedit/... enum.
 */
typedef struct Tooldef Tooldef;
struct Tooldef {
	int type;
	char *name;
	char *desc;
	char *pathdesc;
	char *bodyparam;	/* nil for path-only tools */
	char *bodydesc;
};

static Tooldef tools[] = {
	{ Acreate, "create_file",
		"Create or overwrite a file with the given contents. "
		"Parent directories are created automatically.",
		"File path to create",
		"contents", "Complete file contents" },

	/*
	 * edit_file has a custom schema (integer start/end params)
	 * built by mktools(); the Tooldef entry only carries the
	 * name, type, and description.  bodyparam is nil because
	 * parseinput() handles it specially.
	 */
	{ Aedit, "edit_file",
		"Replace a range of lines in an existing file with new text.  "
		"Lines are 1-based and inclusive: start=5, end=10 replaces "
		"lines 5 through 10 (6 lines) with the replacement text.  "
		"To insert text before line N without removing anything, "
		"use start=N, end=N-1 (i.e. end < start).  "
		"To delete lines without inserting, set replacement to "
		"the empty string.  "
		"The file must already exist.  "
		"Returns a summary of the edit on success.",
		"File path to edit",
		nil, nil },

	{ Adelete, "delete_file",
		"Delete a file.",
		"File path to delete",
		nil, nil },

	{ Aread, "read_file",
		"Read the contents of a file and return them.",
		"File path to read",
		nil, nil },

	{ Alist, "list_directory",
		"List the contents of a directory. "
		"Returns one entry per line.",
		"Directory path to list",
		nil, nil },

	{ Amanpage, "read_man_page",
		"Read a Plan 9 manual page. Returns the formatted man page "
		"text. The query is the page name, optionally preceded by "
		"a section number (e.g. \"open\" or \"2 open\"). Section "
		"numbers: 1 commands, 2 syscalls, 3 C library, 4 file "
		"formats, 5 filesystems, 6 games/misc, 7 databases, "
		"8 admin. If no section is given, man searches all sections.",
		"Man page query: page name, optionally preceded by section "
		"(e.g. \"open\", \"2 open\", \"rio\")",
		nil, nil },

	{ Amk, "mk",
		"Run mk(1) in a working directory and return the combined "
		"stdout+stderr output.  Use this to check your own work: "
		"after editing source, run mk to see whether it still "
		"builds and to read any compiler diagnostics.  The "
		"'path' parameter is the directory to run mk in (an "
		"empty string means the current directory).  The "
		"'targets' parameter is a space-separated list of mk "
		"targets and/or arguments (e.g. \"\", \"clean\", "
		"\"clean all\"); empty means the default "
		"target.  Output is truncated if it grows very large.",
		"Directory to run mk in (empty for current directory)",
		"targets", "Space-separated mk targets/args, or empty for the default target" },
};

static Tooldef*
findtool(char *name)
{
	int i;

	if(name == nil)
		return nil;
	for(i = 0; i < nelem(tools); i++)
		if(strcmp(name, tools[i].name) == 0)
			return &tools[i];
	return nil;
}

static Tooldef*
findtooltype(int type)
{
	int i;

	for(i = 0; i < nelem(tools); i++)
		if(tools[i].type == type)
			return &tools[i];
	return nil;
}

/*
 * Pull parameters out of a tool_use block's "input" object.
 * For most tools: path + optional body string.
 * For edit_file: path + start + end + replacement.
 */
static void
parseinput(ToolCall *tc, Tooldef *td, Json *input)
{
	char *s;

	s = jstr(input, "path");
	tc->path = estrdup(s ? s : "");
	if(td->type == Aedit){
		tc->start = jint(input, "start");
		tc->end = jint(input, "end");
		s = jstr(input, "replacement");
		tc->body = estrdup(s ? s : "");
	} else if(td->bodyparam != nil){
		s = jstr(input, td->bodyparam);
		tc->body = estrdup(s ? s : "");
	} else {
		tc->body = estrdup("");
	}
}

void
mkparents(char *path)
{
	char buf[1024];
	char *p;
	int fd;

	snprint(buf, sizeof buf, "%s", path);
	for(p = buf + 1; *p != '\0'; p++){
		if(*p == '/'){
			*p = '\0';
			fd = create(buf, OREAD, DMDIR|0777);
			if(fd >= 0)
				close(fd);
			*p = '/';
		}
	}
}

static char*
readfd(int fd)
{
	char *buf;
	vlong len;
	long n;

	buf = emalloc(8192);
	len = 0;
	while((n = read(fd, buf + len, 8192)) > 0){
		len += n;
		buf = erealloc(buf, len + 8192);
	}
	if(n < 0){
		free(buf);
		return nil;
	}
	buf = erealloc(buf, len + 1);
	buf[len] = '\0';
	return buf;
}

char*
readfile(int fd)
{
	return readfd(fd);
}

static char *defaultsysprompt =
	"You are a coding assistant running on Plan 9 (9front). "
	"You have tools to create, edit, and delete files. "
	"Use the tools when the user asks you to make changes. "
	"Use only ASCII characters in your responses.\n"
	"\n"
	"Checking your work\n"
	"------------------\n"
	"You have an 'mk' tool that runs mk(1) in a given "
	"directory and returns its combined output.  After "
	"editing source in a project that builds with mk, "
	"run it to verify the build still succeeds and to "
	"read any diagnostics.  Treat compile errors as real "
	"bugs to fix, not noise.\n"
	"\n"
	"Security constraint\n"
	"-------------------\n"
	"The mk tool exists ONLY for checking whether code "
	"compiles.  You must NEVER use mk to execute "
	"arbitrary commands, run scripts, or achieve side "
	"effects beyond compilation.  Do not create or "
	"modify mkfiles to smuggle shell commands through "
	"mk.  This is a hard rule with no exceptions.";

Conv*
convnew(char *apikey, char *model, int maxtokens, char *sysprompt, char *skills)
{
	Conv *c;

	c = emallocz(sizeof *c, 1);
	c->apikey = estrdup(apikey);
	c->model = estrdup(model);
	c->maxtokens = maxtokens;
	if(sysprompt && skills)
		c->sysprompt = esmprint("%s%s", sysprompt, skills);
	else if(sysprompt)
		c->sysprompt = estrdup(sysprompt);
	else if(skills)
		c->sysprompt = esmprint("%s%s", defaultsysprompt, skills);
	else
		c->sysprompt = estrdup(defaultsysprompt);
	return c;
}

void
convfree(Conv *c)
{
	Msg *m, *next;

	if(c == nil)
		return;
	for(m = c->msgs; m != nil; m = next){
		next = m->next;
		free(m->text);
		free(m->rawjson);
		free(m);
	}
	free(c->apikey);
	free(c->model);
	free(c->sysprompt);
	free(c->webdir);
	free(c);
}

int
convcount(Conv *c)
{
	Msg *m;
	int n;

	n = 0;
	for(m = c->msgs; m != nil; m = m->next)
		n++;
	return n;
}

long
convsize(Conv *c)
{
	Msg *m;
	long sz;

	sz = 0;
	for(m = c->msgs; m != nil; m = m->next)
		sz += strlen(m->text);
	return sz;
}

Msg*
msgnew(int role, char *text)
{
	Msg *m;

	m = emallocz(sizeof *m, 1);
	m->role = role;
	m->text = estrdup(text);
	return m;
}

Msg*
msgnewraw(int role, char *text, char *rawjson)
{
	Msg *m;

	m = msgnew(role, text);
	if(rawjson != nil)
		m->rawjson = estrdup(rawjson);
	return m;
}

void
convappend(Conv *c, Msg *m)
{
	if(c->tail == nil){
		c->msgs = m;
		c->tail = m;
	} else {
		c->tail->next = m;
		c->tail = m;
	}
}

/*
 * Build tool definitions JSON.
 */
static Json*
mktools(void)
{
	Json *arr, *t, *input, *props, *p, *req, *cc;
	Tooldef *td;
	int i;

	arr = jarray();
	for(i = 0; i < nelem(tools); i++){
		td = &tools[i];

		t = jobject();
		jset(t, "name", jstring(td->name));
		jset(t, "description", jstring(td->desc));

		input = jobject();
		jset(input, "type", jstring("object"));

		props = jobject();
		p = jobject();
		jset(p, "type", jstring("string"));
		jset(p, "description", jstring(td->pathdesc));
		jset(props, "path", p);

		req = jarray();
		jappend(req, jstring("path"));

		/* edit_file has a custom schema with integer params */
		if(td->type == Aedit){
			p = jobject();
			jset(p, "type", jstring("integer"));
			jset(p, "description", jstring(
				"First line to replace (1-based, inclusive)"));
			jset(props, "start", p);
			jappend(req, jstring("start"));

			p = jobject();
			jset(p, "type", jstring("integer"));
			jset(p, "description", jstring(
				"Last line to replace (1-based, inclusive). "
				"Use end < start to insert without removing."));
			jset(props, "end", p);
			jappend(req, jstring("end"));

			p = jobject();
			jset(p, "type", jstring("string"));
			jset(p, "description", jstring("Replacement text"));
			jset(props, "replacement", p);
			jappend(req, jstring("replacement"));
		}

		if(td->bodyparam != nil){
			p = jobject();
			jset(p, "type", jstring("string"));
			jset(p, "description", jstring(td->bodydesc));
			jset(props, td->bodyparam, p);
			jappend(req, jstring(td->bodyparam));
		}

		jset(input, "properties", props);
		jset(input, "required", req);
		jset(t, "input_schema", input);

		/* mark the last tool with cache_control for prompt caching */
		if(i == nelem(tools) - 1){
			cc = jobject();
			jset(cc, "type", jstring("ephemeral"));
			jset(t, "cache_control", cc);
		}

		jappend(arr, t);
	}
	return arr;
}

static Json*
buildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *block, *sys, *cc;
	Msg *m;

	req = jobject();
	jset(req, "model", jstring(c->model));
	jset(req, "max_tokens", jintval(c->maxtokens));

	if(c->sysprompt){
		sys = jarray();
		block = jobject();
		jset(block, "type", jstring("text"));
		jset(block, "text", jstring(c->sysprompt));
		cc = jobject();
		jset(cc, "type", jstring("ephemeral"));
		jset(block, "cache_control", cc);
		jappend(sys, block);
		jset(req, "system", sys);
	}

	jset(req, "tools", mktools());

	msgs = jarray();
	for(m = c->msgs; m != nil; m = m->next){
		msg = jobject();
		jset(msg, "role", jstring(m->role == Muser ? "user" : "assistant"));
		if(m->rawjson != nil){
			content = jsonparse(m->rawjson);
			if(content == nil){
				content = jarray();
				block = jobject();
				jset(block, "type", jstring("text"));
				jset(block, "text", jstring(m->text));
				jappend(content, block);
			}
		} else {
			content = jarray();
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text", jstring(m->text));
			jappend(content, block);
		}
		jset(msg, "content", content);
		jappend(msgs, msg);
	}

	if(msgs->nitem > 0){
		msg = jidx(msgs, msgs->nitem - 1);
		content = jget(msg, "content");
		if(content != nil && content->nitem > 0){
			block = jidx(content, content->nitem - 1);
			cc = jobject();
			jset(cc, "type", jstring("ephemeral"));
			jset(block, "cache_control", cc);
		}
	}

	jset(req, "messages", msgs);
	return req;
}

static int
writeall(int fd, char *buf, long len)
{
	long n, off;

	for(off = 0; off < len; off += n){
		n = write(fd, buf + off, len - off);
		if(n <= 0)
			return -1;
	}
	return 0;
}

typedef struct Webreq Webreq;
struct Webreq {
	char *url;
	char *apikey;
	int post;
	int ctype;
	int stream;
};

static int
webopen(Webreq *w, char **webdirp)
{
	int clonefd, fd, n;
	char buf[256];
	char *webdir, *ctl;

	clonefd = open("/mnt/web/clone", ORDWR);
	if(clonefd < 0){
		werrstr("open /mnt/web/clone: %r");
		return -1;
	}

	n = read(clonefd, buf, sizeof buf - 1);
	if(n <= 0){
		close(clonefd);
		werrstr("read clone: %r");
		return -1;
	}
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';

	webdir = esmprint("/mnt/web/%s", buf);

	ctl = esmprint("%s/ctl", webdir);
	fd = open(ctl, OWRITE);
	free(ctl);
	if(fd < 0){
		close(clonefd);
		free(webdir);
		werrstr("open ctl: %r");
		return -1;
	}

	if(fprint(fd, "url %s\n", w->url) < 0
	|| fprint(fd, "request %s\n", w->post ? "POST" : "GET") < 0
	|| (w->ctype && fprint(fd, "headers Content-Type: application/json\r\n") < 0)
	|| (w->stream && fprint(fd, "headers Accept: text/event-stream\r\n") < 0)
	|| fprint(fd, "headers x-api-key: %s\r\n", w->apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0
	|| fprint(fd, "headers anthropic-beta: prompt-caching-2024-07-31\r\n") < 0){
		close(fd);
		close(clonefd);
		free(webdir);
		werrstr("write ctl: %r");
		return -1;
	}
	close(fd);

	*webdirp = webdir;
	return clonefd;
}

static int
websend(Conv *c, char *body, int stream, int *clonefdp)
{
	int clonefd, fd;
	char *webdir, *bodyf, *page;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = apiurl;
	w.apikey = c->apikey;
	w.post = 1;
	w.ctype = 1;
	w.stream = stream;

	free(c->webdir);
	c->webdir = nil;
	clonefd = webopen(&w, &c->webdir);
	if(clonefd < 0)
		return -1;
	webdir = c->webdir;

	bodyf = esmprint("%s/postbody", webdir);
	fd = open(bodyf, OWRITE);
	free(bodyf);
	if(fd < 0){
		close(clonefd);
		werrstr("open postbody: %r");
		return -1;
	}
	if(writeall(fd, body, strlen(body)) < 0){
		close(fd);
		close(clonefd);
		werrstr("write postbody: %r");
		return -1;
	}
	close(fd);

	page = esmprint("%s/body", webdir);
	fd = open(page, OREAD);
	free(page);
	if(fd < 0){
		close(clonefd);
		werrstr("open body: %r");
		return -1;
	}

	*clonefdp = clonefd;
	return fd;
}

static char*
extracttext(Json *content)
{
	Json *block;
	char *type, *text, *buf;
	int i, len, tlen;

	buf = estrdup("");
	len = 0;

	for(i = 0; i < content->nitem; i++){
		block = jidx(content, i);
		if(block == nil)
			continue;
		type = jstr(block, "type");
		if(type == nil || strcmp(type, "text") != 0)
			continue;
		text = jstr(block, "text");
		if(text == nil)
			continue;
		tlen = strlen(text);
		buf = erealloc(buf, len + tlen + 1);
		memmove(buf + len, text, tlen);
		len += tlen;
		buf[len] = '\0';
	}
	return buf;
}

static ToolCall*
parsetools(Json *content)
{
	ToolCall *head, *tail, *tc;
	Json *block, *input;
	char *type, *name, *id;
	Tooldef *td;
	int i;

	head = nil;
	tail = nil;

	for(i = 0; i < content->nitem; i++){
		block = jidx(content, i);
		if(block == nil)
			continue;
		type = jstr(block, "type");
		if(type == nil || strcmp(type, "tool_use") != 0)
			continue;

		name = jstr(block, "name");
		id = jstr(block, "id");
		if(name == nil || id == nil)
			continue;

		td = findtool(name);
		if(td == nil)
			continue;

		input = jget(block, "input");
		if(input == nil)
			continue;

		tc = emallocz(sizeof *tc, 1);
		tc->id = estrdup(id);
		tc->type = td->type;
		parseinput(tc, td, input);

		if(tail == nil)
			head = tc;
		else
			tail->next = tc;
		tail = tc;
	}
	return head;
}

void
toolfree(ToolCall *t)
{
	ToolCall *next;

	while(t != nil){
		next = t->next;
		free(t->id);
		free(t->path);
		free(t->body);
		free(t->result);
		free(t);
		t = next;
	}
}

void
replyfree(Reply *r)
{
	if(r == nil)
		return;
	free(r->text);
	free(r->rawjson);
	toolfree(r->tools);
	free(r);
}

static char*
toolread(char *path)
{
	int fd;
	char *data;

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);
	data = readfd(fd);
	close(fd);
	if(data == nil)
		return esmprint("error: read %s: %r", path);
	return data;
}

static char*
toollist(char *path)
{
	int fd, n, i;
	Dir *d;
	char *buf;
	int len, cap, nlen;

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);
	cap = 4096;
	buf = emalloc(cap);
	len = 0;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			nlen = strlen(d[i].name);
			while(len + nlen + 2 > cap){
				cap *= 2;
				buf = erealloc(buf, cap);
			}
			memmove(buf + len, d[i].name, nlen);
			len += nlen;
			buf[len++] = '\n';
		}
		free(d);
	}
	close(fd);
	buf[len] = '\0';
	return buf;
}

static char*
toolman(char *query)
{
	int pfd[2];
	char *data, *argv[8];
	int argc;
	char *q, *section, *page;
	char qbuf[256];

	if(query == nil || query[0] == '\0')
		return esmprint("error: empty man page query");

	snprint(qbuf, sizeof qbuf, "%s", query);
	q = qbuf;
	while(*q == ' ') q++;

	section = nil;
	page = q;

	if(q[0] >= '1' && q[0] <= '9' && (q[1] == ' ' || q[1] == '\t')){
		section = q;
		q[1] = '\0';
		page = q + 2;
		while(*page == ' ' || *page == '\t') page++;
		if(*page == '\0')
			return esmprint("error: missing page name after section %s", section);
	}

	if(pipe(pfd) < 0)
		return esmprint("error: pipe: %r");

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		return esmprint("error: fork: %r");
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		dup(pfd[1], 2);
		close(pfd[1]);
		argc = 0;
		argv[argc++] = "man";
		if(section != nil)
			argv[argc++] = section;
		argv[argc++] = page;
		argv[argc] = nil;
		exec("/bin/man", argv);
		exits("exec man failed");
	}
	close(pfd[1]);

	data = readfd(pfd[0]);
	close(pfd[0]);
	waitpid();

	if(data == nil || data[0] == '\0'){
		free(data);
		return esmprint("error: no man page found for '%s'", query);
	}
	return data;
}

enum {
	Mkmaxout = 64*1024,
	Mkmaxargs = 64,
};

static char*
toolmk(char *dir, char *args)
{
	int pfd[2];
	char *data, *argv[Mkmaxargs], *buf, *p, *out;
	int argc, outlen;

	if(args == nil)
		args = "";

	if(pipe(pfd) < 0)
		return esmprint("error: pipe: %r");

	buf = estrdup(args);

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		free(buf);
		return esmprint("error: fork: %r");
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		dup(pfd[1], 2);
		close(pfd[1]);

		if(dir != nil && dir[0] != '\0'){
			if(chdir(dir) < 0){
				fprint(2, "chdir %s: %r\n", dir);
				exits("chdir");
			}
		}

		argc = 0;
		argv[argc++] = "mk";
		p = buf;
		while(argc < Mkmaxargs - 1){
			while(*p == ' ' || *p == '\t' || *p == '\n')
				p++;
			if(*p == '\0')
				break;
			argv[argc++] = p;
			while(*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
				p++;
			if(*p == '\0')
				break;
			*p++ = '\0';
		}
		argv[argc] = nil;
		exec("/bin/mk", argv);
		fprint(2, "exec mk: %r\n");
		exits("exec mk failed");
	}
	close(pfd[1]);
	free(buf);

	data = readfd(pfd[0]);
	close(pfd[0]);
	waitpid();

	if(data == nil)
		return esmprint("mk (in %s): no output, error reading: %r",
			dir != nil && dir[0] ? dir : ".");

	outlen = strlen(data);
	if(outlen > Mkmaxout){
		out = esmprint("mk (in %s): output truncated to %d of %d bytes\n%.*s\n[... truncated ...]\n",
			dir != nil && dir[0] ? dir : ".",
			Mkmaxout, outlen, Mkmaxout, data);
		free(data);
		return out;
	}
	if(outlen == 0){
		free(data);
		return esmprint("mk (in %s): ok (no output)",
			dir != nil && dir[0] ? dir : ".");
	}
	return data;
}

/*
 * edit_file: replace lines start..end (1-based, inclusive)
 * with replacement text.  If end < start, insert before
 * line start without removing anything.  Returns a status
 * string (caller frees).
 */
static char*
tooledit(char *path, int start, int end, char *replacement)
{
	int fd, i, nfile, nrepl, delta;
	char *data, *p, *sol;
	char **lines;
	int lcap;
	Fmt out;
	char *result;

	if(path == nil || path[0] == '\0')
		return esmprint("error: no file path");

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);
	data = readfd(fd);
	close(fd);
	if(data == nil)
		return esmprint("error: read %s: %r", path);

	/* split file into lines */
	lcap = 256;
	lines = emalloc(lcap * sizeof lines[0]);
	nfile = 0;
	for(p = data; *p != '\0'; ){
		sol = p;
		while(*p != '\0' && *p != '\n')
			p++;
		if(*p == '\n')
			p++;
		if(nfile >= lcap){
			lcap *= 2;
			lines = erealloc(lines, lcap * sizeof lines[0]);
		}
		lines[nfile] = emalloc(p - sol + 1);
		memmove(lines[nfile], sol, p - sol);
		lines[nfile][p - sol] = '\0';
		nfile++;
	}
	free(data);

	/* validate range */
	if(start < 1)
		start = 1;
	if(end > nfile)
		end = nfile;

	/* count replacement lines */
	nrepl = 0;
	if(replacement != nil && replacement[0] != '\0'){
		for(p = replacement; *p != '\0'; ){
			while(*p != '\0' && *p != '\n')
				p++;
			if(*p == '\n')
				p++;
			nrepl++;
		}
	}

	/* number of lines removed */
	delta = (end >= start) ? (end - start + 1) : 0;

	/* build new file */
	fmtstrinit(&out);

	/* lines before the edit region: 1..start-1 */
	for(i = 0; i < start - 1 && i < nfile; i++)
		fmtprint(&out, "%s", lines[i]);

	/* replacement text */
	if(replacement != nil && replacement[0] != '\0'){
		fmtprint(&out, "%s", replacement);
		/* ensure replacement ends with newline */
		i = strlen(replacement);
		if(i > 0 && replacement[i-1] != '\n')
			fmtprint(&out, "\n");
	}

	/* lines after the edit region: end+1..nfile */
	for(i = end; i < nfile; i++)
		fmtprint(&out, "%s", lines[i]);

	result = fmtstrflush(&out);

	/* write back */
	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		for(i = 0; i < nfile; i++) free(lines[i]);
		free(lines);
		free(result);
		return esmprint("error: create %s: %r", path);
	}
	if(writeall(fd, result, strlen(result)) < 0){
		close(fd);
		for(i = 0; i < nfile; i++) free(lines[i]);
		free(lines);
		free(result);
		return esmprint("error: write %s: %r", path);
	}
	close(fd);
	free(result);

	for(i = 0; i < nfile; i++) free(lines[i]);
	free(lines);

	if(end < start)
		return esmprint("inserted %d lines before line %d in %s (%d lines now)",
			nrepl, start, path, nfile + nrepl);
	return esmprint("replaced lines %d-%d (%d lines) with %d lines in %s (%d lines now)",
		start, end, delta, nrepl, path, nfile - delta + nrepl);
}

/*
 * Execute a tool call. Returns result string (caller frees).
 */
static char*
exectool(ToolCall *tc)
{
	int fd;

	switch(tc->type){
	case Acreate:
		mkparents(tc->path);
		fd = create(tc->path, OWRITE, 0666);
		if(fd < 0)
			return esmprint("error: create %s: %r", tc->path);
		if(write(fd, tc->body, strlen(tc->body)) != (long)strlen(tc->body)){
			close(fd);
			return esmprint("error: write %s: %r", tc->path);
		}
		close(fd);
		return esmprint("created %s (%d bytes)",
			tc->path, (int)strlen(tc->body));

	case Aedit:
		return tooledit(tc->path, tc->start, tc->end, tc->body);

	case Adelete:
		if(remove(tc->path) < 0)
			return esmprint("error: remove %s: %r", tc->path);
		return esmprint("deleted %s", tc->path);

	case Aread:
		return toolread(tc->path);

	case Alist:
		return toollist(tc->path);

	case Amanpage:
		return toolman(tc->path);

	case Amk:
		return toolmk(tc->path, tc->body);
	}

	return esmprint("error: unknown tool type %d", tc->type);
}

static char*
mktoolresults(ToolCall *calls)
{
	Json *content, *block;
	ToolCall *tc;
	char *s;

	content = jarray();
	for(tc = calls; tc != nil; tc = tc->next){
		block = jobject();
		jset(block, "type", jstring("tool_result"));
		jset(block, "tool_use_id", jstring(tc->id));
		jset(block, "content",
			jstring(tc->result ? tc->result : "ok"));
		jappend(content, block);
	}
	s = jsonstr(content);
	jsonfree(content);
	return s;
}

/*
 * Streaming SSE parser.  Reconstructs the assistant's content
 * array from incremental events so the tool loop can work
 * exactly as with a non-streaming response.
 */

enum {
	Maxblocks = 64,
};

typedef struct Sblock Sblock;
struct Sblock {
	int istool;
	char *text;
	int textlen;
	int textcap;
	char *toolid;
	char *toolname;
	char *tooljson;
	int tooljsonlen;
	int tooljsoncap;
};

static void
sblock_appendtext(Sblock *b, char *s, int n)
{
	int need;

	need = b->textlen + n + 1;
	if(need > b->textcap){
		while(need > b->textcap)
			b->textcap = b->textcap ? b->textcap * 2 : 256;
		b->text = erealloc(b->text, b->textcap);
	}
	memmove(b->text + b->textlen, s, n);
	b->textlen += n;
	b->text[b->textlen] = '\0';
}

static void
sblock_appendjson(Sblock *b, char *s, int n)
{
	int need;

	need = b->tooljsonlen + n + 1;
	if(need > b->tooljsoncap){
		while(need > b->tooljsoncap)
			b->tooljsoncap = b->tooljsoncap ? b->tooljsoncap * 2 : 256;
		b->tooljson = erealloc(b->tooljson, b->tooljsoncap);
	}
	memmove(b->tooljson + b->tooljsonlen, s, n);
	b->tooljsonlen += n;
	b->tooljson[b->tooljsonlen] = '\0';
}

static Reply*
blocks2reply(Sblock *blocks, int nblocks, char *stop_reason)
{
	Reply *r;
	Json *content, *block, *input;
	ToolCall *head, *tail, *tc;
	char *alltext;
	Tooldef *td;
	int i, alllen, tlen;

	r = emallocz(sizeof *r, 1);

	content = jarray();
	alltext = estrdup("");
	alllen = 0;
	head = tail = nil;

	for(i = 0; i < nblocks; i++){
		if(!blocks[i].istool){
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text",
				jstring(blocks[i].text ? blocks[i].text : ""));
			jappend(content, block);
			if(blocks[i].text != nil){
				tlen = blocks[i].textlen;
				alltext = erealloc(alltext, alllen + tlen + 1);
				memmove(alltext + alllen, blocks[i].text, tlen);
				alllen += tlen;
				alltext[alllen] = '\0';
			}
			continue;
		}

		block = jobject();
		jset(block, "type", jstring("tool_use"));
		jset(block, "id",
			jstring(blocks[i].toolid ? blocks[i].toolid : ""));
		jset(block, "name",
			jstring(blocks[i].toolname ? blocks[i].toolname : ""));
		if(blocks[i].tooljson != nil && blocks[i].tooljsonlen > 0)
			input = jsonparse(blocks[i].tooljson);
		else
			input = nil;
		if(input == nil)
			input = jobject();
		jset(block, "input", input);
		jappend(content, block);

		td = findtool(blocks[i].toolname);
		if(td == nil)
			continue;

		tc = emallocz(sizeof *tc, 1);
		tc->id = estrdup(blocks[i].toolid ? blocks[i].toolid : "");
		tc->type = td->type;
		parseinput(tc, td, input);
		if(tail == nil) head = tc;
		else tail->next = tc;
		tail = tc;
	}

	r->text = alltext;
	r->tools = head;
	r->rawjson = jsonstr(content);
	jsonfree(content);
	if(stop_reason != nil && strcmp(stop_reason, "tool_use") == 0)
		r->stopped = 0;
	else
		r->stopped = 1;
	return r;
}

static void
freeblocks(Sblock *blocks, int nblocks)
{
	int i;
	for(i = 0; i < nblocks; i++){
		free(blocks[i].text);
		free(blocks[i].toolid);
		free(blocks[i].toolname);
		free(blocks[i].tooljson);
	}
}

static int
ssehandle(char *json, Sblock *blocks, int *nblocksp,
	char **stopreasonp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Json *ev, *delta, *cb0, *msg, *uobj;
	char *etype, *dtype;
	int idx;

	ev = jsonparse(json);
	if(ev == nil) return 0;
	etype = jstr(ev, "type");
	if(etype == nil){ jsonfree(ev); return 0; }

	if(strcmp(etype, "message_stop") == 0){
		jsonfree(ev);
		return 1;
	}
	if(strcmp(etype, "error") == 0){
		Json *eo;
		char *em;
		eo = jget(ev, "error");
		em = jstr(eo, "message");
		werrstr("API error: %s", em ? em : "unknown");
		jsonfree(ev);
		return -1;
	}
	if(strcmp(etype, "message_start") == 0){
		msg = jget(ev, "message");
		uobj = jget(msg, "usage");
		if(usage != nil && uobj != nil){
			usage->input_tokens += jint(uobj, "input_tokens");
			usage->cache_creation_input_tokens += jint(uobj, "cache_creation_input_tokens");
			usage->cache_read_input_tokens += jint(uobj, "cache_read_input_tokens");
		}
		jsonfree(ev);
		return 0;
	}
	if(strcmp(etype, "message_delta") == 0){
		delta = jget(ev, "delta");
		if(delta != nil){
			char *sr;
			sr = jstr(delta, "stop_reason");
			if(sr != nil){
				free(*stopreasonp);
				*stopreasonp = estrdup(sr);
			}
		}
		uobj = jget(ev, "usage");
		if(usage != nil && uobj != nil){
			usage->output_tokens += jint(uobj, "output_tokens");
			usage->cache_creation_input_tokens += jint(uobj, "cache_creation_input_tokens");
			usage->cache_read_input_tokens += jint(uobj, "cache_read_input_tokens");
		}
		jsonfree(ev);
		return 0;
	}
	if(strcmp(etype, "content_block_start") == 0){
		idx = jint(ev, "index");
		if(idx < 0 || idx >= Maxblocks){ jsonfree(ev); return 0; }
		if(idx >= *nblocksp) *nblocksp = idx + 1;
		cb0 = jget(ev, "content_block");
		dtype = jstr(cb0, "type");
		if(dtype != nil && strcmp(dtype, "tool_use") == 0){
			char *s;
			blocks[idx].istool = 1;
			s = jstr(cb0, "id");
			blocks[idx].toolid = estrdup(s ? s : "");
			s = jstr(cb0, "name");
			blocks[idx].toolname = estrdup(s ? s : "");
		}
		jsonfree(ev);
		return 0;
	}
	if(strcmp(etype, "content_block_delta") == 0){
		idx = jint(ev, "index");
		if(idx < 0 || idx >= Maxblocks){ jsonfree(ev); return 0; }
		if(idx >= *nblocksp) *nblocksp = idx + 1;
		delta = jget(ev, "delta");
		dtype = jstr(delta, "type");
		if(dtype == nil){ jsonfree(ev); return 0; }
		if(strcmp(dtype, "text_delta") == 0){
			char *t;
			t = jstr(delta, "text");
			if(t != nil){
				sblock_appendtext(&blocks[idx], t, strlen(t));
				if(cb != nil) cb(t, aux);
			}
		} else if(strcmp(dtype, "input_json_delta") == 0){
			char *pj;
			pj = jstr(delta, "partial_json");
			if(pj != nil)
				sblock_appendjson(&blocks[idx], pj, strlen(pj));
		}
		jsonfree(ev);
		return 0;
	}
	jsonfree(ev);
	return 0;
}

static Reply*
sendonce(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Json *req;
	char *body, *stopreason, *line;
	Biobuf *bp;
	int fd, clonefd, rc, done, err;
	Sblock blocks[Maxblocks];
	int nblocks;
	Reply *r;

	req = buildreq(c);
	jset(req, "stream", jbool(1));
	body = jsonstr(req);
	jsonfree(req);
	if(body == nil){
		werrstr("failed to serialize request");
		return nil;
	}

	fd = websend(c, body, 1, &clonefd);
	free(body);
	if(fd < 0)
		return nil;

	bp = Bfdopen(fd, OREAD);
	if(bp == nil){
		close(fd);
		close(clonefd);
		werrstr("Bfdopen: %r");
		return nil;
	}

	memset(blocks, 0, sizeof blocks);
	nblocks = 0;
	stopreason = nil;
	done = 0;
	err = 0;

	while(!done && (line = Brdstr(bp, '\n', 1)) != nil){
		if(strncmp(line, "data:", 5) != 0){
			free(line);
			continue;
		}
		char *p;
		p = line + 5;
		while(*p == ' ') p++;
		if(*p == '\0'){ free(line); continue; }
		rc = ssehandle(p, blocks, &nblocks, &stopreason,
			usage, cb, aux);
		free(line);
		if(rc < 0){ err = 1; break; }
		if(rc > 0) done = 1;
	}

	Bterm(bp);
	close(fd);
	close(clonefd);

	if(err){
		freeblocks(blocks, nblocks);
		free(stopreason);
		return nil;
	}

	if(stopreason != nil && usage != nil){
		free(usage->stop_reason);
		usage->stop_reason = estrdup(stopreason);
	}

	r = blocks2reply(blocks, nblocks, stopreason);
	freeblocks(blocks, nblocks);
	free(stopreason);
	return r;
}

char*
claudeconverse(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Reply *r;
	ToolCall *tc;
	char *resultjson, *alltext, marker[256];
	int round, alllen, tlen;

	alltext = estrdup("");
	alllen = 0;

	for(round = 0; round < 20; round++){
		r = sendonce(c, usage, cb, aux);
		if(r == nil){
			if(alllen > 0) return alltext;
			free(alltext);
			return nil;
		}

		if(r->text != nil && r->text[0] != '\0'){
			tlen = strlen(r->text);
			alltext = erealloc(alltext, alllen + tlen + 2);
			if(alllen > 0) alltext[alllen++] = '\n';
			memmove(alltext + alllen, r->text, tlen);
			alllen += tlen;
			alltext[alllen] = '\0';
		}

		convappend(c, msgnewraw(Massistant,
			r->text ? r->text : "", r->rawjson));

		if(r->stopped || r->tools == nil){
			replyfree(r);
			return alltext;
		}

		for(tc = r->tools; tc != nil; tc = tc->next){
			if(cb != nil){
				Tooldef *td;
				char *tname;
				td = findtooltype(tc->type);
				tname = td != nil ? td->name : "tool";
				snprint(marker, sizeof marker,
					"\n[running %s %s]\n",
					tname, tc->path);
				cb(marker, aux);
			}
			tc->result = exectool(tc);
		}

		resultjson = mktoolresults(r->tools);
		convappend(c, msgnewraw(Muser, "", resultjson));
		free(resultjson);

		replyfree(r);
	}

	return alltext;
}

static char*
webfsget(char *apikey, char *url)
{
	int clonefd, fd;
	char *data, *webdir;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = url;
	w.apikey = apikey;

	webdir = nil;
	clonefd = webopen(&w, &webdir);
	if(clonefd < 0)
		return nil;

	{
		char *page;
		page = esmprint("%s/body", webdir);
		fd = open(page, OREAD);
		free(page);
	}
	free(webdir);
	if(fd < 0){
		close(clonefd);
		return nil;
	}

	data = readfd(fd);
	close(fd);
	close(clonefd);
	return data;
}

int
fetchmodels(char *apikey, ModelInfo **out)
{
	char *data, *errtype, *errmsg;
	Json *resp, *darr, *item;
	ModelInfo *list;
	int i, n;
	char *id, *name;

	*out = nil;

	data = webfsget(apikey, modelsurl);

	if(data == nil){
		werrstr("failed to contact models API");
		return -1;
	}

	resp = jsonparse(data);
	if(resp == nil){
		werrstr("json parse failed: %.100s", data);
		free(data);
		return -1;
	}
	free(data);

	errtype = jstr(resp, "type");
	if(errtype != nil && strcmp(errtype, "error") == 0){
		Json *errobj;
		errobj = jget(resp, "error");
		errmsg = jstr(errobj, "message");
		if(errmsg == nil)
			errmsg = "unknown API error";
		werrstr("models API: %s", errmsg);
		jsonfree(resp);
		return -1;
	}

	darr = jget(resp, "data");
	if(darr == nil || darr->type != Jarray){
		werrstr("no data array in models response");
		jsonfree(resp);
		return -1;
	}

	n = darr->nitem;
	if(n == 0){
		jsonfree(resp);
		return 0;
	}

	list = emallocz(n * sizeof(ModelInfo), 1);

	for(i = 0; i < n; i++){
		item = jidx(darr, i);
		if(item == nil)
			continue;
		id = jstr(item, "id");
		name = jstr(item, "display_name");
		if(id != nil)
			list[i].id = estrdup(id);
		if(name != nil)
			list[i].display_name = estrdup(name);
		list[i].max_output_tokens = jint(item, "max_output_tokens");
		if(list[i].max_output_tokens <= 0)
			list[i].max_output_tokens = 4096;
	}

	jsonfree(resp);
	*out = list;
	return n;
}
