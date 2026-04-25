#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

static char *apiurl = "https://api.anthropic.com/v1/messages";
static char *modelsurl = "https://api.anthropic.com/v1/models?limit=100";
static char *apiversion = "2023-06-01";

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
		"The diff is applied by an in-tree fuzzy matcher that "
		"anchors each hunk by its context lines, so:\n"
		"  - '--- a/path' / '+++ b/path' headers are optional "
		"(the target file is given by the path parameter);\n"
		"  - line numbers in '@@ -a,b +c,d @@' are used as hints "
		"but do not have to be exact;\n"
		"  - a bare '@@' with no line numbers is accepted;\n"
		"  - whitespace-only differences inside a line are tolerated.\n"
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
	tc->path = strdup(s ? s : "");
	if(td->bodyparam != nil){
		s = jstr(input, td->bodyparam);
		tc->body = strdup(s ? s : "");
	} else {
		tc->body = strdup("");
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
 */
char*
readfd(int fd)
{
	char *buf, *tmp;
	vlong len;
	long n;

	buf = malloc(8192);
	if(buf == nil)
		return nil;
	len = 0;
	while((n = read(fd, buf + len, 8192)) > 0){
		len += n;
		tmp = realloc(buf, len + 8192);
		if(tmp == nil){
			free(buf);
			return nil;
		}
		buf = tmp;
	}
	if(n < 0){
		free(buf);
		return nil;
	}
	tmp = realloc(buf, len + 1);
	if(tmp == nil){
		free(buf);
		return nil;
	}
	buf = tmp;
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

	c = mallocz(sizeof *c, 1);
	if(c == nil)
		sysfatal("malloc: %r");
	c->apikey = strdup(apikey);
	c->model = strdup(model);
	c->maxtokens = maxtokens;
	if(sysprompt)
		c->sysprompt = strdup(sysprompt);
	else
		c->sysprompt = strdup(
			"You are a coding assistant running on Plan 9 (9front). "
			"You have tools to create, patch, and delete files. "
			"Use the tools when the user asks you to make changes. "
			"Use only ASCII characters in your responses.");
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

	m = mallocz(sizeof *m, 1);
	if(m == nil)
		sysfatal("malloc: %r");
	m->role = role;
	m->text = strdup(text);
	if(m->text == nil)
		sysfatal("strdup: %r");
	return m;
}

Msg*
msgnewraw(int role, char *text, char *rawjson)
{
	Msg *m;

	m = msgnew(role, text);
	if(rawjson != nil)
		m->rawjson = strdup(rawjson);
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
	Json *arr, *t, *input, *props, *p, *req;
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
		jappend(arr, t);
	}
	return arr;
}

static Json*
buildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *block;
	Msg *m;

	req = jobject();
	jset(req, "model", jstring(c->model));
	jset(req, "max_tokens", jintval(c->maxtokens));

	if(c->sysprompt)
		jset(req, "system", jstring(c->sysprompt));

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

static char*
webfssend(Conv *c, char *body)
{
	int clonefd, fd, n;
	char buf[256], *data;
	char *clone, *ctl, *bodyf, *page;

	clone = "/mnt/web/clone";
	clonefd = open(clone, ORDWR);
	if(clonefd < 0){
		werrstr("open %s: %r", clone);
		return nil;
	}

	n = read(clonefd, buf, sizeof buf - 1);
	if(n <= 0){
		close(clonefd);
		werrstr("read clone: %r");
		return nil;
	}
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';

	free(c->webdir);
	c->webdir = smprint("/mnt/web/%s", buf);

	ctl = smprint("%s/ctl", c->webdir);
	fd = open(ctl, OWRITE);
	free(ctl);
	if(fd < 0){
		close(clonefd);
		werrstr("open ctl: %r");
		return nil;
	}

	if(fprint(fd, "url %s\n", apiurl) < 0
	|| fprint(fd, "request POST\n") < 0
	|| fprint(fd, "headers Content-Type: application/json\r\n") < 0
	|| fprint(fd, "headers x-api-key: %s\r\n", c->apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0){
		close(fd);
		close(clonefd);
		werrstr("write ctl: %r");
		return nil;
	}
	close(fd);

	bodyf = smprint("%s/postbody", c->webdir);
	fd = open(bodyf, OWRITE);
	free(bodyf);
	if(fd < 0){
		close(clonefd);
		werrstr("open postbody: %r");
		return nil;
	}
	if(writeall(fd, body, strlen(body)) < 0){
		close(fd);
		close(clonefd);
		werrstr("write postbody: %r");
		return nil;
	}
	close(fd);

	page = smprint("%s/body", c->webdir);
	fd = open(page, OREAD);
	free(page);
	if(fd < 0){
		close(clonefd);
		werrstr("open body: %r");
		return nil;
	}

	data = readfd(fd);
	close(fd);
	close(clonefd);
	if(data == nil)
		werrstr("empty response from API");
	return data;
}

static char*
curlconfig(char *apikey)
{
	char *path;
	int fd;

	path = smprint("/tmp/claude9.cfg.%d", getpid());
	fd = create(path, OWRITE|OEXCL, 0600);
	if(fd < 0){
		free(path);
		return nil;
	}
	fprint(fd, "header \"Content-Type: application/json\"\n");
	fprint(fd, "header \"x-api-key: %s\"\n", apikey);
	fprint(fd, "header \"anthropic-version: %s\"\n", apiversion);
	close(fd);
	return path;
}

static char*
curlsend(Conv *c, char *body)
{
	int pfd[2], fd;
	char *data, *cfg, *bodypath, *bodyarg;

	cfg = curlconfig(c->apikey);
	if(cfg == nil){
		werrstr("curlconfig: %r");
		return nil;
	}

	bodypath = smprint("/tmp/claude9.body.%d", getpid());
	fd = create(bodypath, OWRITE|OEXCL, 0600);
	if(fd < 0){
		remove(cfg);
		free(cfg);
		free(bodypath);
		werrstr("create body tmpfile: %r");
		return nil;
	}
	if(writeall(fd, body, strlen(body)) < 0){
		close(fd);
		remove(bodypath);
		free(bodypath);
		remove(cfg);
		free(cfg);
		werrstr("write body tmpfile: %r");
		return nil;
	}
	close(fd);

	bodyarg = smprint("@%s", bodypath);

	if(pipe(pfd) < 0){
		remove(bodypath);
		free(bodypath);
		free(bodyarg);
		remove(cfg);
		free(cfg);
		werrstr("pipe: %r");
		return nil;
	}

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		remove(bodypath);
		free(bodypath);
		free(bodyarg);
		remove(cfg);
		free(cfg);
		werrstr("fork: %r");
		return nil;
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		close(pfd[1]);
		execl("/usr/bin/curl", "curl", "-s",
			"-X", "POST",
			"-K", cfg,
			"-d", bodyarg,
			apiurl,
			nil);
		sysfatal("exec curl: %r");
	}
	close(pfd[1]);

	data = readfd(pfd[0]);
	close(pfd[0]);
	waitpid();

	remove(bodypath);
	free(bodypath);
	free(bodyarg);
	remove(cfg);
	free(cfg);

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
	int clonefd, fd, n;
	char buf[256];
	char *clone, *ctl, *bodyf, *page;

	clone = "/mnt/web/clone";
	clonefd = open(clone, ORDWR);
	if(clonefd < 0){
		werrstr("open %s: %r", clone);
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

	free(c->webdir);
	c->webdir = smprint("/mnt/web/%s", buf);

	ctl = smprint("%s/ctl", c->webdir);
	fd = open(ctl, OWRITE);
	free(ctl);
	if(fd < 0){
		close(clonefd);
		werrstr("open ctl: %r");
		return -1;
	}
	if(fprint(fd, "url %s\n", apiurl) < 0
	|| fprint(fd, "request POST\n") < 0
	|| fprint(fd, "headers Content-Type: application/json\r\n") < 0
	|| fprint(fd, "headers Accept: text/event-stream\r\n") < 0
	|| fprint(fd, "headers x-api-key: %s\r\n", c->apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0){
		close(fd);
		close(clonefd);
		werrstr("write ctl: %r");
		return -1;
	}
	close(fd);

	bodyf = smprint("%s/postbody", c->webdir);
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

	page = smprint("%s/body", c->webdir);
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

static int
haswebfs(void)
{
	return access("/mnt/web/clone", AREAD) == 0;
}

static char*
extracttext(Json *content)
{
	Json *block;
	char *type, *text, *buf, *tmp;
	int i, len, tlen;

	buf = strdup("");
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
		tmp = realloc(buf, len + tlen + 1);
		if(tmp == nil)
			sysfatal("realloc: %r");
		buf = tmp;
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

		tc = mallocz(sizeof *tc, 1);
		if(tc == nil)
			sysfatal("malloc: %r");
		tc->id = strdup(id);
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

	if(haswebfs())
		data = webfssend(c, body);
	else
		data = curlsend(c, body);
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
		}
	}

	content = jget(resp, "content");
	if(content == nil || content->type != Jarray){
		jsonfree(resp);
		werrstr("no content in response");
		return nil;
	}

	r = mallocz(sizeof *r, 1);
	if(r == nil)
		sysfatal("malloc: %r");

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
			usage->stop_reason = strdup(stopreason->str);
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
		return smprint("error: open %s: %r", path);
	data = readfd(fd);
	close(fd);
	if(data == nil)
		return smprint("error: read %s: %r", path);
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
	char *buf, *tmp;
	int len, cap, nlen;

	fd = open(path, OREAD);
	if(fd < 0)
		return smprint("error: open %s: %r", path);
	cap = 4096;
	buf = malloc(cap);
	if(buf == nil)
		sysfatal("malloc: %r");
	len = 0;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			nlen = strlen(d[i].name);
			while(len + nlen + 2 > cap){
				cap *= 2;
				tmp = realloc(buf, cap);
				if(tmp == nil)
					sysfatal("realloc: %r");
				buf = tmp;
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
			return smprint("error: create %s: %r", tc->path);
		if(write(fd, tc->body, strlen(tc->body)) != (long)strlen(tc->body)){
			close(fd);
			return smprint("error: write %s: %r", tc->path);
		}
		close(fd);
		return smprint("created %s (%d bytes)",
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
			return smprint("error: remove %s: %r", tc->path);
		return smprint("deleted %s", tc->path);

	case Aread:
		return toolread(tc->path);

	case Alist:
		return toollist(tc->path);
	}

	return smprint("error: unknown tool type %d", tc->type);
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
	char *resultjson, *alltext, *tmp;
	int round, alllen, tlen;

	alltext = strdup("");
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
			tmp = realloc(alltext, alllen + tlen + 2);
			if(tmp == nil)
				sysfatal("realloc: %r");
			alltext = tmp;
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
		b->text = realloc(b->text, b->textcap);
		if(b->text == nil) sysfatal("realloc: %r");
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
		b->tooljson = realloc(b->tooljson, b->tooljsoncap);
		if(b->tooljson == nil) sysfatal("realloc: %r");
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
	char *alltext, *tmp;
	Tooldef *td;
	int i, alllen, tlen;

	r = mallocz(sizeof *r, 1);
	if(r == nil) sysfatal("malloc: %r");

	/* build rawjson content array */
	content = jarray();
	alltext = strdup("");
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
				tmp = realloc(alltext, alllen + tlen + 1);
				if(tmp == nil) sysfatal("realloc: %r");
				alltext = tmp;
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

		tc = mallocz(sizeof *tc, 1);
		if(tc == nil) sysfatal("malloc: %r");
		tc->id = strdup(blocks[i].toolid ? blocks[i].toolid : "");
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
		if(usage != nil && uobj != nil)
			usage->input_tokens += jint(uobj, "input_tokens");
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
				*stopreasonp = strdup(sr);
			}
		}
		uobj = jget(ev, "usage");
		if(usage != nil && uobj != nil)
			usage->output_tokens += jint(uobj, "output_tokens");
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
			blocks[idx].toolid = strdup(s ? s : "");
			s = jstr(cb0, "name");
			blocks[idx].toolname = strdup(s ? s : "");
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
		usage->stop_reason = strdup(stopreason);
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
	char *resultjson, *alltext, *tmp, marker[256];
	int round, alllen, tlen;

	/*
	 * Streaming requires webfs (body file delivers bytes as
	 * they arrive).  On systems without webfs (e.g. plan9port
	 * with only curl), fall back to the non-streaming path --
	 * the caller still gets a complete reply, just all at once
	 * when the round finishes.  We invoke the callback with
	 * the entire final text so readers of the stream file
	 * see the reply too (just not incrementally).
	 */
	if(!haswebfs()){
		alltext = claudeconverse(c, usage);
		if(alltext != nil && alltext[0] != '\0' && cb != nil)
			cb(alltext, aux);
		return alltext;
	}

	alltext = strdup("");
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
			tmp = realloc(alltext, alllen + tlen + 2);
			if(tmp == nil) sysfatal("realloc: %r");
			alltext = tmp;
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

static char*
webfsget(char *apikey, char *url)
{
	int clonefd, fd, n;
	char buf[256], *data;
	char *clone, *ctl, *page, *webdir;

	clone = "/mnt/web/clone";
	clonefd = open(clone, ORDWR);
	if(clonefd < 0)
		return nil;

	n = read(clonefd, buf, sizeof buf - 1);
	if(n <= 0){
		close(clonefd);
		return nil;
	}
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';

	webdir = smprint("/mnt/web/%s", buf);

	ctl = smprint("%s/ctl", webdir);
	fd = open(ctl, OWRITE);
	free(ctl);
	if(fd < 0){
		close(clonefd);
		free(webdir);
		return nil;
	}

	if(fprint(fd, "url %s\n", url) < 0
	|| fprint(fd, "request GET\n") < 0
	|| fprint(fd, "headers x-api-key: %s\r\n", apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0){
		close(fd);
		close(clonefd);
		free(webdir);
		return nil;
	}
	close(fd);

	page = smprint("%s/body", webdir);
	fd = open(page, OREAD);
	free(page);
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

static char*
curlget(char *apikey, char *url)
{
	int pfd[2];
	char *data, *cfg;

	cfg = curlconfig(apikey);
	if(cfg == nil)
		return nil;

	if(pipe(pfd) < 0){
		remove(cfg);
		free(cfg);
		return nil;
	}

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		remove(cfg);
		free(cfg);
		return nil;
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		close(pfd[1]);
		execl("/usr/bin/curl", "curl", "-s",
			"-K", cfg,
			url,
			nil);
		sysfatal("exec curl: %r");
	}
	close(pfd[1]);

	data = readfd(pfd[0]);
	close(pfd[0]);
	waitpid();

	remove(cfg);
	free(cfg);
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

	if(haswebfs())
		data = webfsget(apikey, modelsurl);
	else
		data = curlget(apikey, modelsurl);

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

	list = mallocz(n * sizeof(ModelInfo), 1);
	if(list == nil)
		sysfatal("malloc: %r");

	for(i = 0; i < n; i++){
		item = jidx(darr, i);
		if(item == nil)
			continue;
		id = jstr(item, "id");
		name = jstr(item, "display_name");
		if(id != nil)
			list[i].id = strdup(id);
		if(name != nil)
			list[i].display_name = strdup(name);
		list[i].max_output_tokens = jint(item, "max_output_tokens");
		if(list[i].max_output_tokens <= 0)
			list[i].max_output_tokens = 4096;
	}

	jsonfree(resp);
	*out = list;
	return n;
}
