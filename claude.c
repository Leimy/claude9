#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

static char *apiurl = "https://api.anthropic.com/v1/messages";
static char *modelsurl = "https://api.anthropic.com/v1/models?limit=100";
static char *apiversion = "2023-06-01";

static char *toolnames[] = {
	"create_file",
	"patch_file",
	"delete_file",
	"read_file",
	"list_directory",
};
static int tooltypes[] = {
	Acreate,
	Apatch,
	Adelete,
	Aread,
	Alist,
};

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
	Json *tools, *t, *input, *props, *p, *req;

	tools = jarray();

	/* create_file(path, contents) */
	t = jobject();
	jset(t, "name", jstring("create_file"));
	jset(t, "description", jstring(
		"Create or overwrite a file with the given contents. "
		"Parent directories are created automatically."));
	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("File path to create"));
	jset(props, "path", p);
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("Complete file contents"));
	jset(props, "contents", p);
	jset(input, "properties", props);
	req = jarray();
	jappend(req, jstring("path"));
	jappend(req, jstring("contents"));
	jset(input, "required", req);
	jset(t, "input_schema", input);
	jappend(tools, t);

	/* patch_file(path, diff) */
	t = jobject();
	jset(t, "name", jstring("patch_file"));
	jset(t, "description", jstring(
		"Apply a unified diff patch to an existing file."));
	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("File path to patch"));
	jset(props, "path", p);
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("Unified diff to apply"));
	jset(props, "diff", p);
	jset(input, "properties", props);
	req = jarray();
	jappend(req, jstring("path"));
	jappend(req, jstring("diff"));
	jset(input, "required", req);
	jset(t, "input_schema", input);
	jappend(tools, t);

	/* read_file(path) */
	t = jobject();
	jset(t, "name", jstring("read_file"));
	jset(t, "description", jstring(
		"Read the contents of a file and return them."));
	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("File path to read"));
	jset(props, "path", p);
	jset(input, "properties", props);
	req = jarray();
	jappend(req, jstring("path"));
	jset(input, "required", req);
	jset(t, "input_schema", input);
	jappend(tools, t);

	/* list_directory(path) */
	t = jobject();
	jset(t, "name", jstring("list_directory"));
	jset(t, "description", jstring(
		"List the contents of a directory. "
		"Returns one entry per line."));
	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("Directory path to list"));
	jset(props, "path", p);
	jset(input, "properties", props);
	req = jarray();
	jappend(req, jstring("path"));
	jset(input, "required", req);
	jset(t, "input_schema", input);
	jappend(tools, t);

	/* delete_file(path) */
	t = jobject();
	jset(t, "name", jstring("delete_file"));
	jset(t, "description", jstring("Delete a file."));
	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	p = jobject();
	jset(p, "type", jstring("string"));
	jset(p, "description", jstring("File path to delete"));
	jset(props, "path", p);
	jset(input, "properties", props);
	req = jarray();
	jappend(req, jstring("path"));
	jset(input, "required", req);
	jset(t, "input_schema", input);
	jappend(tools, t);

	return tools;
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
	char *type, *name, *id, *s;
	int i, j, ttype;

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

		ttype = -1;
		for(j = 0; j < nelem(toolnames); j++){
			if(strcmp(name, toolnames[j]) == 0){
				ttype = tooltypes[j];
				break;
			}
		}
		if(ttype < 0)
			continue;

		input = jget(block, "input");
		if(input == nil)
			continue;

		tc = mallocz(sizeof *tc, 1);
		if(tc == nil)
			sysfatal("malloc: %r");
		tc->id = strdup(id);
		tc->type = ttype;

		switch(ttype){
		case Acreate:
			s = jstr(input, "path");
			tc->path = strdup(s ? s : "");
			s = jstr(input, "contents");
			tc->body = strdup(s ? s : "");
			break;
		case Apatch:
			s = jstr(input, "path");
			tc->path = strdup(s ? s : "");
			s = jstr(input, "diff");
			tc->body = strdup(s ? s : "");
			break;
		case Adelete:
		case Aread:
		case Alist:
			s = jstr(input, "path");
			tc->path = strdup(s ? s : "");
			tc->body = strdup("");
			break;
		}

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
		{
		char *tmp, *patchout;
		int tfd, pfd[2], n;
		char outbuf[1024];

		tmp = smprint("/tmp/claude9.patch.%d", getpid());
		tfd = create(tmp, OWRITE, 0600);
		if(tfd < 0){
			free(tmp);
			return smprint("error: create temp: %r");
		}
		write(tfd, tc->body, strlen(tc->body));
		close(tfd);

		if(pipe(pfd) < 0){
			remove(tmp);
			free(tmp);
			return smprint("error: pipe: %r");
		}

		switch(fork()){
		case -1:
			close(pfd[0]);
			close(pfd[1]);
			remove(tmp);
			free(tmp);
			return smprint("error: fork: %r");
		case 0:
			close(pfd[0]);
			dup(pfd[1], 1);
			dup(pfd[1], 2);
			close(pfd[1]);
			execl("/bin/ape/patch", "patch",
				tc->path, tmp, nil);
			execl("/bin/patch", "patch",
				tc->path, tmp, nil);
			exits("exec");
		}
		close(pfd[1]);

		n = read(pfd[0], outbuf, sizeof outbuf - 1);
		if(n < 0) n = 0;
		outbuf[n] = '\0';
		close(pfd[0]);
		waitpid();

		remove(tmp);
		free(tmp);

		if(n > 0)
			patchout = smprint("patched %s: %s",
				tc->path, outbuf);
		else
			patchout = smprint("patched %s", tc->path);
		return patchout;
		}

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
mktoolresults(ToolCall *tools)
{
	Json *content, *block;
	ToolCall *tc;
	char *s;

	content = jarray();
	for(tc = tools; tc != nil; tc = tc->next){
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

char*
claudesend(Conv *c, Usage *usage)
{
	Reply *r;
	char *text;

	r = sendonce(c, usage);
	if(r == nil)
		return nil;
	text = r->text;
	r->text = nil;
	replyfree(r);
	return text;
}

Reply*
claudechat(Conv *c, Usage *usage)
{
	return sendonce(c, usage);
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
