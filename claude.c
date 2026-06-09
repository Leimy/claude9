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
 * Most tools take a "path" parameter plus an optional body
 * string (bodyparam/bodydesc).  replace_string is special:
 * it has old_str and new_str parameters, handled via custom
 * code in mktools() and parseinput().  findtool() maps API
 * names to the Acreate/Areplace/... enum.
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
	 * replace_string has a custom schema (old_str, new_str)
	 * built by mktools(); bodyparam is nil because parseinput()
	 * handles it specially.
	 */
	{ Areplace, "replace_string",
		"Replace the first exact match of old_str with new_str "
		"in a file.  The old_str must match exactly one location "
		"in the file.  If it matches zero times, an error is "
		"returned (check for typos or stale text).  If it matches "
		"more than once, an error is returned (include more "
		"surrounding context in old_str to make it unique).  "
		"To delete text, set new_str to the empty string.  "
		"The file must already exist.  "
		"Returns a summary of the replacement on success.",
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

/*
 * Pull parameters out of a tool_use block's "input" object.
 * For most tools: path + optional body string.
 * For replace_string: path + old_str + new_str.
 */
static void
parseinput(ToolCall *tc, Tooldef *td, Json *input)
{
	char *s;

	s = jstr(input, "path");
	tc->path = estrdup(s ? s : "");
	if(td->type == Areplace){
		s = jstr(input, "old_str");
		tc->oldstr = estrdup(s ? s : "");
		s = jstr(input, "new_str");
		tc->newstr = estrdup(s ? s : "");
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
	char *buf, *p;
	int fd;

	if(path == nil || path[0] == '\0')
		return;
	buf = estrdup(path);
	for(p = buf + 1; *p != '\0'; p++){
		if(*p == '/'){
			*p = '\0';
			fd = create(buf, OREAD, DMDIR|0777);
			if(fd >= 0)
				close(fd);
			*p = '/';
		}
	}
	free(buf);
}

char*
readfile(int fd)
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
	if(sysprompt == nil)
		sysprompt = defaultsysprompt;
	if(skills != nil)
		c->sysprompt = esmprint("%s%s", sysprompt, skills);
	else
		c->sysprompt = estrdup(sysprompt);
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

		/* replace_string has a custom schema */
		if(td->type == Areplace){
			p = jobject();
			jset(p, "type", jstring("string"));
			jset(p, "description", jstring(
				"The exact text to search for in the file. "
				"Must match exactly once."));
			jset(props, "old_str", p);
			jappend(req, jstring("old_str"));

			p = jobject();
			jset(p, "type", jstring("string"));
			jset(p, "description", jstring(
				"The replacement text. Use empty string to delete."));
			jset(props, "new_str", p);
			jappend(req, jstring("new_str"));
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

/*
 * Wrap a plain text string in a JSON content array:
 *   [{"type":"text","text":"..."}]
 */
static Json*
mktextcontent(char *text)
{
	Json *content, *block;

	content = jarray();
	block = jobject();
	jset(block, "type", jstring("text"));
	jset(block, "text", jstring(text));
	jappend(content, block);
	return content;
}

static Json*
buildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *block, *sys, *cc;
	Msg *m;

	req = jobject();
	jset(req, "model", jstring(c->model));
	jset(req, "max_tokens", jintval(c->maxtokens));

	/*
	 * Extended thinking.  budget_tokens must be < max_tokens;
	 * clamp rather than let the API reject the request.
	 */
	if(c->thinking > 0){
		Json *think;
		int budget;

		budget = c->thinking;
		if(budget < 1024)
			budget = 1024;
		if(budget >= c->maxtokens)
			budget = c->maxtokens / 2;
		think = jobject();
		if(budget >= 1024){
			jset(think, "type", jstring("enabled"));
			jset(think, "budget_tokens", jintval(budget));
			jset(req, "thinking", think);
		} else
			jsonfree(think);
	}

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
		content = nil;
		if(m->rawjson != nil)
			content = jsonparse(m->rawjson);
		/*
		 * A raw content array can end up empty (e.g. an
		 * assistant turn whose only text blocks were empty
		 * and were skipped).  The API rejects empty content
		 * arrays, so fall through to the placeholder.
		 */
		if(content != nil && content->type == Jarray
		&& content->nitem == 0){
			jsonfree(content);
			content = nil;
		}
		if(content == nil){
			/*
			 * Guard against empty text content blocks:
			 * the API rejects {"type":"text","text":""}.
			 * Use a single space as a harmless placeholder
			 * when the message text is empty.
			 */
			if(m->text == nil || m->text[0] == '\0')
				content = mktextcontent(" ");
			else
				content = mktextcontent(m->text);
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
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0){
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
	char *webdir, *path;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = apiurl;
	w.apikey = c->apikey;
	w.post = 1;
	w.ctype = 1;
	w.stream = stream;

	clonefd = webopen(&w, &webdir);
	if(clonefd < 0)
		return -1;

	path = esmprint("%s/postbody", webdir);
	fd = open(path, OWRITE);
	free(path);
	if(fd < 0){
		close(clonefd);
		free(webdir);
		werrstr("open postbody: %r");
		return -1;
	}
	if(writeall(fd, body, strlen(body)) < 0){
		close(fd);
		close(clonefd);
		free(webdir);
		werrstr("write postbody: %r");
		return -1;
	}
	close(fd);

	path = esmprint("%s/body", webdir);
	fd = open(path, OREAD);
	free(path);
	if(fd < 0){
		char *ebody, *emsg;
		int efd;

		/*
		 * HTTP error.  Read the errorbody file from webfs
		 * to get the server's response, which for the
		 * Anthropic API is JSON with a detailed message.
		 */
		path = esmprint("%s/errorbody", webdir);
		efd = open(path, OREAD);
		free(path);
		free(webdir);
		if(efd >= 0){
			ebody = readfile(efd);
			close(efd);
		} else
			ebody = nil;
		if(ebody != nil){
			Json *ej, *eo;
			ej = jsonparse(ebody);
			emsg = nil;
			if(ej != nil){
				eo = jget(ej, "error");
				if(eo != nil)
					emsg = jstr(eo, "message");
			}
			if(emsg != nil)
				werrstr("API error: %s", emsg);
			else
				werrstr("API error: %s", ebody);
			jsonfree(ej);
			free(ebody);
		} else
			werrstr("open body: %r");
		close(clonefd);
		return -1;
	}
	free(webdir);

	*clonefdp = clonefd;
	return fd;
}

void
toolfree(ToolCall *t)
{
	ToolCall *next;

	while(t != nil){
		next = t->next;
		free(t->id);
		free(t->name);
		free(t->path);
		free(t->body);
		free(t->oldstr);
		free(t->newstr);
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
	data = readfile(fd);
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
	Fmt f;

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);
	fmtstrinit(&f);
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++)
			fmtprint(&f, "%s\n", d[i].name);
		free(d);
	}
	close(fd);
	return fmtstrflush(&f);
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

	data = readfile(pfd[0]);
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

	data = readfile(pfd[0]);
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
 * replace_string: find old_str in the file, verify it occurs
 * exactly once, replace it with new_str.  Returns a status
 * string (caller frees).
 *
 * This is the content-addressed edit model used by Claude Code's
 * str_replace_editor.  It is safe against stale state: if the
 * file has changed and the old text is no longer present, the
 * operation fails loudly instead of silently corrupting.
 */
static char*
toolreplace(char *path, char *oldstr, char *newstr)
{
	int fd, count;
	long filelen, oldlen, newlen;
	char *data, *p, *match, *result;

	if(path == nil || path[0] == '\0')
		return esmprint("error: no file path");
	if(oldstr == nil || oldstr[0] == '\0')
		return esmprint("error: old_str is empty");

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);
	data = readfile(fd);
	close(fd);
	if(data == nil)
		return esmprint("error: read %s: %r", path);

	filelen = strlen(data);
	oldlen = strlen(oldstr);
	newlen = (newstr != nil) ? strlen(newstr) : 0;

	/* count occurrences of old_str */
	count = 0;
	match = nil;
	for(p = data; (p = strstr(p, oldstr)) != nil; p += oldlen){
		if(count == 0)
			match = p;
		count++;
	}

	if(count == 0){
		free(data);
		return esmprint("error: old_str not found in %s", path);
	}
	if(count > 1){
		free(data);
		return esmprint("error: old_str matches %d times in %s; "
			"include more context to make it unique", count, path);
	}

	/* build replacement: prefix + new_str + suffix */
	result = emalloc(filelen - oldlen + newlen + 1);
	memmove(result, data, match - data);
	if(newlen > 0)
		memmove(result + (match - data), newstr, newlen);
	memmove(result + (match - data) + newlen,
		match + oldlen,
		filelen - (match - data) - oldlen);
	result[filelen - oldlen + newlen] = '\0';

	free(data);

	/* write back */
	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		free(result);
		return esmprint("error: create %s: %r", path);
	}
	if(writeall(fd, result, strlen(result)) < 0){
		close(fd);
		free(result);
		return esmprint("error: write %s: %r", path);
	}
	close(fd);
	free(result);

	if(newlen == 0)
		return esmprint("deleted %ld bytes in %s", oldlen, path);
	return esmprint("replaced %ld bytes with %ld bytes in %s",
		oldlen, newlen, path);
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
		if(writeall(fd, tc->body, strlen(tc->body)) < 0){
			close(fd);
			return esmprint("error: write %s: %r", tc->path);
		}
		close(fd);
		return esmprint("created %s (%d bytes)",
			tc->path, (int)strlen(tc->body));

	case Areplace:
		return toolreplace(tc->path, tc->oldstr, tc->newstr);

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

	return esmprint("error: unknown tool '%s'",
		tc->name ? tc->name : "");
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
	int isthinking;
	char *text;
	int textlen;
	int textcap;
	/* thinking blocks: streamed text + closing signature */
	char *thinking;
	int thinkinglen;
	int thinkingcap;
	char *sig;
	int siglen;
	int sigcap;
	char *redacted;		/* redacted_thinking: opaque data blob */
	char *toolid;
	char *toolname;
	char *tooljson;
	int tooljsonlen;
	int tooljsoncap;
};

/*
 * Append n bytes of s to a growable buffer.
 */
static void
sbappend(char **buf, int *len, int *cap, char *s, int n)
{
	int need;

	need = *len + n + 1;
	if(need > *cap){
		while(need > *cap)
			*cap = *cap ? *cap * 2 : 256;
		*buf = erealloc(*buf, *cap);
	}
	memmove(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
}

static Reply*
blocks2reply(Sblock *blocks, int nblocks, char *stop_reason)
{
	Reply *r;
	Json *content, *block, *input;
	ToolCall *head, *tail, *tc;
	Tooldef *td;
	Fmt f;
	int i;

	r = emallocz(sizeof *r, 1);

	content = jarray();
	fmtstrinit(&f);
	head = tail = nil;

	for(i = 0; i < nblocks; i++){
		if(blocks[i].isthinking){
			/*
			 * Thinking blocks must be passed back verbatim
			 * (with signature) when the turn continues with
			 * tool results, or the API rejects the request.
			 */
			block = jobject();
			if(blocks[i].redacted != nil){
				jset(block, "type", jstring("redacted_thinking"));
				jset(block, "data", jstring(blocks[i].redacted));
			} else {
				jset(block, "type", jstring("thinking"));
				jset(block, "thinking",
					jstring(blocks[i].thinking ? blocks[i].thinking : ""));
				jset(block, "signature",
					jstring(blocks[i].sig ? blocks[i].sig : ""));
			}
			jappend(content, block);
			continue;
		}
		if(!blocks[i].istool){
			/* skip empty text blocks: the API rejects them */
			if(blocks[i].text == nil || blocks[i].text[0] == '\0')
				continue;
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text", jstring(blocks[i].text));
			jappend(content, block);
			fmtprint(&f, "%s", blocks[i].text);
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

		/*
		 * Always create a ToolCall, even for an unknown tool
		 * name: every tool_use block in the assistant content
		 * must get a matching tool_result in the next user
		 * message, or the API rejects the whole conversation.
		 * Unknown tools get type -1 and exectool returns an
		 * error result for them.
		 */
		td = findtool(blocks[i].toolname);
		tc = emallocz(sizeof *tc, 1);
		tc->id = estrdup(blocks[i].toolid ? blocks[i].toolid : "");
		tc->name = estrdup(blocks[i].toolname ? blocks[i].toolname : "");
		if(td != nil){
			tc->type = td->type;
			parseinput(tc, td, input);
		} else {
			tc->type = -1;
			tc->path = estrdup("");
			tc->body = estrdup("");
		}
		if(tail == nil) head = tc;
		else tail->next = tc;
		tail = tc;
	}

	r->text = fmtstrflush(&f);
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
		free(blocks[i].thinking);
		free(blocks[i].sig);
		free(blocks[i].redacted);
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
		} else if(dtype != nil && strcmp(dtype, "thinking") == 0){
			blocks[idx].isthinking = 1;
			if(cb != nil) cb("[thinking]\n", aux);
		} else if(dtype != nil && strcmp(dtype, "redacted_thinking") == 0){
			char *s;
			blocks[idx].isthinking = 1;
			s = jstr(cb0, "data");
			blocks[idx].redacted = estrdup(s ? s : "");
			if(cb != nil) cb("[redacted thinking]\n", aux);
		}
		jsonfree(ev);
		return 0;
	}
	if(strcmp(etype, "content_block_stop") == 0){
		idx = jint(ev, "index");
		if(idx >= 0 && idx < Maxblocks
		&& blocks[idx].isthinking && blocks[idx].redacted == nil
		&& cb != nil)
			cb("\n[/thinking]\n", aux);
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
				sbappend(&blocks[idx].text,
					&blocks[idx].textlen,
					&blocks[idx].textcap,
					t, strlen(t));
				if(cb != nil) cb(t, aux);
			}
		} else if(strcmp(dtype, "input_json_delta") == 0){
			char *pj;
			pj = jstr(delta, "partial_json");
			if(pj != nil)
				sbappend(&blocks[idx].tooljson,
					&blocks[idx].tooljsonlen,
					&blocks[idx].tooljsoncap,
					pj, strlen(pj));
		} else if(strcmp(dtype, "thinking_delta") == 0){
			char *t;
			t = jstr(delta, "thinking");
			if(t != nil){
				sbappend(&blocks[idx].thinking,
					&blocks[idx].thinkinglen,
					&blocks[idx].thinkingcap,
					t, strlen(t));
				if(cb != nil) cb(t, aux);
			}
		} else if(strcmp(dtype, "signature_delta") == 0){
			char *t;
			t = jstr(delta, "signature");
			if(t != nil)
				sbappend(&blocks[idx].sig,
					&blocks[idx].siglen,
					&blocks[idx].sigcap,
					t, strlen(t));
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
				snprint(marker, sizeof marker,
					"\n[running %s %s]\n",
					tc->name, tc->path);
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
	char *data, *webdir, *page;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = url;
	w.apikey = apikey;

	webdir = nil;
	clonefd = webopen(&w, &webdir);
	if(clonefd < 0)
		return nil;

	page = esmprint("%s/body", webdir);
	fd = open(page, OREAD);
	free(page);
	free(webdir);
	if(fd < 0){
		close(clonefd);
		return nil;
	}

	data = readfile(fd);
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
