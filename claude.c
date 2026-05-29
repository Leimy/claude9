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
 * second parameter (file contents, unified diff) declare it in
 * the bodyparam / bodydesc fields.  mktools() turns this table
 * into the Anthropic tools schema, parseinput() extracts the
 * path/body fields out of a tool_use block's input JSON, and
 * findtool() / findtooltype() let the rest of the code walk
 * between API names and the Acreate/Apatch/... enum.
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

	{ Apatch, "patch_file",
		"Apply a unified diff to an existing file.\n"
		"\n"
		"Accepts standard unified diff syntax with one or more "
		"hunks. Each hunk starts with a '@@' header and contains "
		"lines prefixed ' ' (context), '-' (remove), or '+' (add).\n"
		"\n"
		"The diff is applied by patch(1), which anchors each "
		"hunk by its context lines, so:\n"
		"  - '--- a/path' / '+++ b/path' headers are optional "
		"(the target file is given by the path parameter);\n"
		"  - line numbers in '@@ -a,b +c,d @@' are used as hints "
		"but do not have to be exact.\n"
		"\n"
		"Include at least one unchanged context line before and "
		"after each edit so the hunk can be located unambiguously. "
		"Hunks are applied in order.",
		"File path to patch",
		"diff", "Unified diff to apply" },

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

	{ Amemcheck, "memory_check",
		"Report on the persistent memory store (beadsfs).  "
		"Returns whether memory is mounted, the total bead count, "
		"the type distribution, and a preview of the most recent "
		"beads.  Memory is a graph of small notes ('beads') with "
		"typed directed edges that survives across sessions.  "
		"Beads live at the given mount point (default /n/beads); "
		"interact with them using the file tools: "
		"read_file /n/beads/by-id/N/info to inspect a bead, "
		"list_directory /n/beads/by-id to enumerate, "
		"create_file /n/beads/new to add a bead, "
		"create_file /n/beads/link 'FROM EDGETYPE TO' to link two beads, "
		"create_file /n/beads/by-id/N/ctl 'type X' to retype, "
		"delete_file /n/beads/by-id/N to remove a bead and its edges.",
		"Memory mount point (use /n/beads unless told otherwise)",
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
		"\"clean all\", \"beadsfs\"); empty means the default "
		"target.  Output is truncated if it grows very large.",
		"Directory to run mk in (empty for current directory)",
		"targets", "Space-separated mk targets/args, or empty for the default target" },
};

/*
 * Look up a tool by its API name.  Returns nil if name is not
 * one of ours.
 */
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
 * Look up a tool by its internal Acreate/Apatch/... type.
 * Returns nil if no match.
 */
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
 * Pull "path" and (if applicable) the body parameter out of a
 * tool_use block's "input" object, copying both into the given
 * ToolCall.  Always sets path and body to non-nil malloc'd
 * strings, possibly empty.
 */
static void
parseinput(ToolCall *tc, Tooldef *td, Json *input)
{
	char *s;

	s = jstr(input, "path");
	tc->path = estrdup(s ? s : "");
	if(td->bodyparam != nil){
		s = jstr(input, td->bodyparam);
		tc->body = estrdup(s ? s : "");
	} else {
		tc->body = estrdup("");
	}
}

/*
 * Create parent directories for path, like mkdir -p.
 */
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

/*
 * Read all data from fd into a malloc'd NUL-terminated string.
 * Returns nil on error (deliberate -- callers propagate the failure).
 */
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

Conv*
convnew(char *apikey, char *model, int maxtokens, char *sysprompt)
{
	Conv *c;

	c = emallocz(sizeof *c, 1);
	c->apikey = estrdup(apikey);
	c->model = estrdup(model);
	c->maxtokens = maxtokens;
	if(sysprompt)
		c->sysprompt = estrdup(sysprompt);
	else
		c->sysprompt = estrdup(
			"You are a coding assistant running on Plan 9 (9front). "
			"You have tools to create, patch, and delete files. "
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
			"Environment\n"
			"-----------\n"
			"You may have a persistent memory store mounted at "
			"/n/beads, served by beadsfs.  Use the memory_check "
			"tool at the start of a session to see if it is "
			"available and to learn what is already there.  If "
			"it is, treat it as your project notebook across "
			"sessions.\n"
			"\n"
			"Memory is a graph of small notes called 'beads'.  "
			"Each bead has a numeric id, a one-letter type "
			"(t=task, n=note, d=decision, a=artifact, l=log), "
			"and a payload of up to about 500 bytes.  Beads can "
			"be linked with typed directed edges "
			"(d=depends, r=references, c=child, s=supersedes).\n"
			"\n"
			"Interact with memory using the normal file tools:\n"
			"  list_directory /n/beads/by-id          -- list all beads\n"
			"  read_file /n/beads/by-id/N/info        -- inspect a bead\n"
			"  read_file /n/beads/by-id/N/payload     -- raw payload\n"
			"  list_directory /n/beads/search/<q>     -- search by substring\n"
			"  list_directory /n/beads/by-type/<t>    -- filter by type letter\n"
			"  create_file /n/beads/new <content>     -- create a note bead\n"
			"  create_file /n/beads/link 'A d B'      -- link bead A to bead B (depends)\n"
			"  create_file /n/beads/by-id/N/payload   -- replace payload\n"
			"  create_file /n/beads/by-id/N/ctl 'type X' -- retype to X\n"
			"  delete_file /n/beads/by-id/N           -- delete bead (and its edges)\n"
			"\n"
			"Memory etiquette:\n"
			"  - At session start, run memory_check.  If beads "
			"exist, browse for context relevant to the user's "
			"request before making changes.\n"
			"  - When you finish a meaningful chunk of work or "
			"make a decision worth remembering, write a short "
			"bead summarising it and link it to whatever "
			"motivated the work.\n"
			"  - Aim for one idea per bead; a bead should be a "
			"sentence or two, not an essay.  If a thought "
			"doesn't fit, split it across linked beads using "
			"child ('c') edges.\n"
			"  - If /n/beads is not mounted, proceed without it "
			"and do not mention it to the user.");
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

	/*
	 * Mark the last message's content with cache_control so the
	 * entire conversation prefix is cached on subsequent requests.
	 */
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

/*
 * Describes one HTTP request to webfs.  The boolean flags pick
 * the few headers that differ between our three call sites:
 *   post   - method is POST (vs GET); a postbody file is written
 *   ctype  - send "Content-Type: application/json"
 *   stream - send "Accept: text/event-stream"
 */
typedef struct Webreq Webreq;
struct Webreq {
	char *url;
	char *apikey;
	int post;
	int ctype;
	int stream;
};

/*
 * Open a fresh webfs connection for the given request: clone a
 * connection, work out its directory, then write the url,
 * method, and headers to its ctl file.  On success returns the
 * open clone fd (which must stay open for the lifetime of the
 * request) and stores a malloc'd connection directory path in
 * *webdirp (caller frees).  On failure returns -1 with werrstr
 * set and leaves *webdirp untouched.
 */
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

/*
 * Write the POST body to a connection's postbody file.
 * Returns 0 on success, -1 with werrstr set on failure.
 */
static int
webpost(char *webdir, char *body)
{
	int fd;
	char *bodyf;

	bodyf = esmprint("%s/postbody", webdir);
	fd = open(bodyf, OWRITE);
	free(bodyf);
	if(fd < 0){
		werrstr("open postbody: %r");
		return -1;
	}
	if(writeall(fd, body, strlen(body)) < 0){
		close(fd);
		werrstr("write postbody: %r");
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Open a connection's body file for reading the response.
 * Returns the open fd, or -1 with werrstr set.
 */
static int
webbody(char *webdir)
{
	int fd;
	char *page;

	page = esmprint("%s/body", webdir);
	fd = open(page, OREAD);
	free(page);
	if(fd < 0)
		werrstr("open body: %r");
	return fd;
}

/*
 * POST a request body to the messages API and return the whole
 * response as a malloc'd string (caller frees), or nil with
 * werrstr set.
 */
static char*
webfssend(Conv *c, char *body)
{
	int clonefd, fd;
	char *data;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = apiurl;
	w.apikey = c->apikey;
	w.post = 1;
	w.ctype = 1;

	free(c->webdir);
	c->webdir = nil;
	clonefd = webopen(&w, &c->webdir);
	if(clonefd < 0)
		return nil;

	if(webpost(c->webdir, body) < 0){
		close(clonefd);
		return nil;
	}

	fd = webbody(c->webdir);
	if(fd < 0){
		close(clonefd);
		return nil;
	}

	data = readfd(fd);
	close(fd);
	close(clonefd);
	if(data == nil)
		werrstr("empty response from API");
	return data;
}

/*
 * Streaming variant of webfssend.  Performs the same clone /
 * ctl / postbody dance as webfssend but leaves the body file
 * open for incremental reads and returns the open fd.
 * Webfs's body file hands out response bytes as they arrive
 * from the HTTP socket, so reading it line by line gives us
 * Server-Sent Events in real time.
 *
 * On success, returns the read fd of the body file and stores
 * the clone fd in *clonefdp -- the caller must close both
 * (body fd first, then clone fd) when done to release the
 * webfs connection.
 *
 * On failure returns -1 with werrstr set.
 */
static int
webfssend_stream(Conv *c, char *body, int *clonefdp)
{
	int clonefd, fd;
	Webreq w;

	memset(&w, 0, sizeof w);
	w.url = apiurl;
	w.apikey = c->apikey;
	w.post = 1;
	w.ctype = 1;
	w.stream = 1;

	free(c->webdir);
	c->webdir = nil;
	clonefd = webopen(&w, &c->webdir);
	if(clonefd < 0)
		return -1;

	if(webpost(c->webdir, body) < 0){
		close(clonefd);
		return -1;
	}

	fd = webbody(c->webdir);
	if(fd < 0){
		close(clonefd);
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

static Reply*
sendonce(Conv *c, Usage *usage)
{
	Json *req, *resp, *content, *stopreason, *uobj;
	char *body, *data, *errtype, *errmsg;
	Reply *r;

	req = buildreq(c);
	body = jsonstr(req);
	jsonfree(req);
	if(body == nil){
		werrstr("failed to serialize request");
		return nil;
	}

	data = webfssend(c, body);
	free(body);

	if(data == nil){
		werrstr("no response: %r");
		return nil;
	}

	resp = jsonparse(data);
	if(resp == nil){
		werrstr("json parse failed: %.100s", data);
		free(data);
		return nil;
	}
	free(data);

	errtype = jstr(resp, "type");
	if(errtype != nil && strcmp(errtype, "error") == 0){
		Json *errobj;
		errobj = jget(resp, "error");
		errmsg = jstr(errobj, "message");
		if(errmsg == nil)
			errmsg = "unknown API error";
		werrstr("API error: %s", errmsg);
		jsonfree(resp);
		return nil;
	}

	if(usage != nil){
		uobj = jget(resp, "usage");
		if(uobj != nil){
			usage->input_tokens += jint(uobj, "input_tokens");
			usage->output_tokens += jint(uobj, "output_tokens");
			usage->cache_creation_input_tokens += jint(uobj, "cache_creation_input_tokens");
			usage->cache_read_input_tokens += jint(uobj, "cache_read_input_tokens");
		}
	}

	content = jget(resp, "content");
	if(content == nil || content->type != Jarray){
		jsonfree(resp);
		werrstr("no content in response");
		return nil;
	}

	r = emallocz(sizeof *r, 1);
	r->text = extracttext(content);
	r->tools = parsetools(content);
	r->rawjson = jsonstr(content);

	stopreason = jget(resp, "stop_reason");
	if(stopreason != nil && stopreason->type == Jstring){
		if(strcmp(stopreason->str, "tool_use") == 0)
			r->stopped = 0;
		else
			r->stopped = 1;
		if(usage != nil){
			free(usage->stop_reason);
			usage->stop_reason = estrdup(stopreason->str);
		}
	} else {
		r->stopped = 1;
	}

	jsonfree(resp);
	return r;
}

/*
 * Read a file, return contents. Caller frees.
 */
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

/*
 * List a directory, return one name per line. Caller frees.
 */
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

/*
 * Read a man page by forking man(1).
 * The query string is the path field, e.g. "open" or "2 open".
 * On 9front, man takes positional args: man [section] page.
 * Returns the formatted text. Caller frees.
 */
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

	/* parse optional section number from query */
	snprint(qbuf, sizeof qbuf, "%s", query);
	q = qbuf;
	while(*q == ' ') q++;

	section = nil;
	page = q;

	/*
	 * If the first token is a single digit followed by
	 * whitespace, treat it as a man section.  We terminate the
	 * digit in place (q[1] = '\0') so that "section" points at
	 * just the one-character string "N", and advance "page"
	 * past the separator to the page name.
	 */
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

/*
 * memory_check: report on a beadsfs mount.
 * path is the mount root (e.g. /n/beads); empty -> /n/beads.
 * Returns a human-readable summary:
 *   - mount status
 *   - bead count
 *   - type distribution (one line per type letter)
 *   - last few beads with id, type, payload preview
 * Always returns a malloc'd string.  Errors are reported in
 * the returned text so Claude sees them as tool output.
 */
static char*
toolmemcheck(char *root)
{
	char *mount, *byid, *p, *out;
	int fd, n, i, total, npreview, previewmax;
	Dir *d;
	vlong *ids;
	int nids, idcap;
	int counts[128];
	char line[1024];
	int outlen, outcap;

	if(root == nil || root[0] == '\0')
		mount = estrdup("/n/beads");
	else
		mount = estrdup(root);

	byid = esmprint("%s/by-id", mount);
	fd = open(byid, OREAD);
	if(fd < 0){
		out = esmprint("memory not mounted at %s: %r\n"
			"hint: beadsfs -s beads -m %s <store>\n",
			mount, mount);
		free(mount);
		free(byid);
		return out;
	}

	/* enumerate ids */
	ids = nil;
	nids = 0;
	idcap = 0;
	memset(counts, 0, sizeof counts);
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(nids >= idcap){
				idcap = idcap ? idcap * 2 : 64;
				ids = erealloc(ids, idcap * sizeof *ids);
			}
			ids[nids++] = strtoll(d[i].name, nil, 10);
		}
		free(d);
	}
	close(fd);
	total = nids;

	/* read each bead's type to build type histogram */
	for(i = 0; i < nids; i++){
		char *tpath;
		int tfd, m;
		char tb[16];
		tpath = esmprint("%s/by-id/%lld/type", mount, ids[i]);
		tfd = open(tpath, OREAD);
		free(tpath);
		if(tfd < 0)
			continue;
		m = read(tfd, tb, sizeof tb - 1);
		close(tfd);
		if(m <= 0)
			continue;
		tb[m] = '\0';
		p = tb;
		while(*p == ' ' || *p == '\n') p++;
		if(*p != '\0' && (uchar)*p < 128)
			counts[(uchar)*p]++;
	}

	/* build output */
	outcap = 4096;
	out = emalloc(outcap);
	outlen = 0;
	outlen += snprint(out + outlen, outcap - outlen,
		"memory mounted at %s\n", mount);
	outlen += snprint(out + outlen, outcap - outlen,
		"beads: %d\n", total);
	if(total > 0){
		outlen += snprint(out + outlen, outcap - outlen, "types:\n");
		for(i = 0; i < 128; i++){
			if(counts[i] == 0)
				continue;
			outlen += snprint(out + outlen, outcap - outlen,
				"  %c %d\n", i, counts[i]);
		}
	}

	/* sort ids ascending (dirread order isn't guaranteed) */
	for(i = 1; i < nids; i++){
		vlong v = ids[i];
		int j = i - 1;
		while(j >= 0 && ids[j] > v){
			ids[j+1] = ids[j];
			j--;
		}
		ids[j+1] = v;
	}

	/* preview last 5 by id */
	previewmax = 5;
	npreview = total < previewmax ? total : previewmax;
	if(npreview > 0){
		outlen += snprint(out + outlen, outcap - outlen,
			"recent:\n");
		for(i = nids - npreview; i < nids; i++){
			char *ipath, *info;
			int ifd, m;
			char ibuf[1024];
			ipath = esmprint("%s/by-id/%lld/info", mount, ids[i]);
			ifd = open(ipath, OREAD);
			free(ipath);
			if(ifd < 0)
				continue;
			m = read(ifd, ibuf, sizeof ibuf - 1);
			close(ifd);
			if(m <= 0)
				continue;
			ibuf[m] = '\0';
			/* trim to first 160 chars and strip trailing newline */
			if(m > 160){
				ibuf[160] = '\0';
				m = 160;
			}
			while(m > 0 && (ibuf[m-1] == '\n' || ibuf[m-1] == ' '))
				ibuf[--m] = '\0';
			/* collapse newlines inside info for compactness */
			info = ibuf;
			for(p = info; *p != '\0'; p++)
				if(*p == '\n') *p = ' ';
			/* grow out as needed */
			snprint(line, sizeof line, "  %s\n", info);
			n = strlen(line);
			while(outlen + n + 1 > outcap){
				outcap *= 2;
				out = erealloc(out, outcap);
			}
			memmove(out + outlen, line, n);
			outlen += n;
			out[outlen] = '\0';
		}
	}

	free(ids);
	free(byid);
	free(mount);
	return out;
}

/*
 * toolmk: run mk(1) in the given directory and return its
 * combined stdout+stderr.  dir == nil or "" means cwd.  args
 * is a single string of space-separated mk arguments/targets;
 * we split on runs of whitespace.  Output is capped so a
 * runaway build can't blow the conversation budget.
 */
enum {
	Mkmaxout = 64*1024,	/* truncate beyond this many bytes */
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

	/* duplicate args so we can chop it up in place */
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

	case Apatch:
		/*
		 * Use the in-tree fuzzy unified-diff applier
		 * (patch.c:applydiff). Tolerant of off-by-one line
		 * numbers, missing --- / +++ headers, and minor
		 * whitespace drift.
		 */
		return applydiff(tc->path, tc->body);

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

	case Amemcheck:
		return toolmemcheck(tc->path);

	case Amk:
		return toolmk(tc->path, tc->body);
	}

	return esmprint("error: unknown tool type %d", tc->type);
}

/*
 * Build a tool_result user message JSON content array.
 */
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
 * Full tool loop: send, execute tools, send results, repeat.
 * Appends all messages to the conversation.
 * Returns final assistant text (caller frees).
 */
char*
claudeconverse(Conv *c, Usage *usage)
{
	Reply *r;
	ToolCall *tc;
	char *resultjson, *alltext;
	int round, alllen, tlen;

	alltext = estrdup("");
	alllen = 0;

	for(round = 0; round < 20; round++){
		r = sendonce(c, usage);
		if(r == nil){
			if(alllen > 0)
				return alltext;
			free(alltext);
			return nil;
		}

		/* accumulate text */
		if(r->text != nil && r->text[0] != '\0'){
			tlen = strlen(r->text);
			alltext = erealloc(alltext, alllen + tlen + 2);
			if(alllen > 0)
				alltext[alllen++] = '\n';
			memmove(alltext + alllen, r->text, tlen);
			alllen += tlen;
			alltext[alllen] = '\0';
		}

		/* add assistant message with raw content */
		convappend(c, msgnewraw(Massistant,
			r->text ? r->text : "", r->rawjson));

		if(r->stopped || r->tools == nil){
			replyfree(r);
			return alltext;
		}

		/* execute each tool */
		for(tc = r->tools; tc != nil; tc = tc->next)
			tc->result = exectool(tc);

		/* build tool_result message */
		resultjson = mktoolresults(r->tools);
		convappend(c, msgnewraw(Muser, "", resultjson));
		free(resultjson);

		replyfree(r);
	}

	return alltext;
}

/*
 * Streaming path.
 *
 * We reuse buildreq() but set "stream":true, then parse
 * Server-Sent Events from webfs's body file.  For each
 * incremental text delta we invoke cb(chunk, aux).  The assistant's full
 * content array is reconstructed locally (text blocks and
 * tool_use blocks with their JSON input) so that the existing
 * tool-loop logic still works: we build a Reply just like
 * sendonce() would have, and let claudeconverse_stream drive
 * the rounds.
 *
 * Only a small subset of the SSE event stream matters to us:
 *
 *   content_block_start  -- announces a new block; if type is
 *                           tool_use, remember id/name/index
 *   content_block_delta  -- text_delta carries text to emit;
 *                           input_json_delta carries partial
 *                           tool input JSON to accumulate
 *   content_block_stop   -- finalize current block
 *   message_delta        -- carries stop_reason and usage
 *   message_stop         -- end of message
 *   error / overloaded_error -- propagate as werrstr
 *
 * Anything else is ignored.
 */

enum {
	Maxblocks = 64,
};

typedef struct Sblock Sblock;
struct Sblock {
	int istool;
	/* text block */
	char *text;
	int textlen;
	int textcap;
	/* tool_use block */
	char *toolid;
	char *toolname;
	char *tooljson;	/* accumulated input_json_delta partials */
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

/*
 * Convert accumulated Sblocks into a Reply (text + tools +
 * rawjson) shaped exactly like sendonce() returns.
 */
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

	/* build rawjson content array */
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

		/* tool_use */
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

/*
 * Parse a single SSE "data: ..." JSON payload and update state.
 * Returns 0 normally, -1 on API error (werrstr set), 1 on
 * message_stop.
 */
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
	/* content_block_stop, ping, etc. -- ignore */
	jsonfree(ev);
	return 0;
}

static Reply*
sendonce_stream(Conv *c, Usage *usage,
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

	fd = webfssend_stream(c, body, &clonefd);
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
		/* SSE: we only care about "data: ..." lines */
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
claudeconverse_stream(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Reply *r;
	ToolCall *tc;
	char *resultjson, *alltext, marker[256];
	int round, alllen, tlen;

	alltext = estrdup("");
	alllen = 0;

	for(round = 0; round < 20; round++){
		r = sendonce_stream(c, usage, cb, aux);
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
			/* emit a marker so the user sees why text paused */
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

/*
 * GET a URL from the API and return the response as a malloc'd
 * string (caller frees), or nil with werrstr set.
 */
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

	fd = webbody(webdir);
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
