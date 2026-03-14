#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

static char *apiurl = "https://api.anthropic.com/v1/messages";
static char *modelsurl = "https://api.anthropic.com/v1/models?limit=100";
static char *apiversion = "2023-06-01";

/*
 * Read all data from fd into a malloc'd NUL-terminated string.
 * Returns nil on error (with errstr set).
 * Caller must free the result.
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
			"When you need to create or modify files, use action blocks:\n"
			"<<<ACTION file:create path:filename\n"
			"contents\n"
			">>>ACTION\n"
			"Use file:delete to remove files. Always provide complete file contents for create.\n"
			"Use file:patch with unified diff format to modify existing files:\n"
			"<<<ACTION file:patch path:filename\n"
			"--- a/filename\n"
			"+++ b/filename\n"
			"@@ -start,count +start,count @@\n"
			" context line\n"
			"-removed line\n"
			"+added line\n"
			">>>ACTION\n"
			"Prefer file:patch over file:create when making small changes to existing files.\n\n"
			"IMPORTANT: Only use action blocks when you intend to actually create, modify, or delete files. "
			"Never use action block markers when discussing or explaining the syntax. "
			"If you need to reference the format, describe it in prose or use different delimiters like quotes.\n"
			"\n"
			"Use only ASCII characters in your responses. Do not use Unicode dashes, quotes, arrows, or other non-ASCII characters.");
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

	msgs = jarray();
	for(m = c->msgs; m != nil; m = m->next){
		msg = jobject();
		jset(msg, "role", jstring(m->role == Muser ? "user" : "assistant"));
		content = jarray();
		block = jobject();
		jset(block, "type", jstring("text"));
		jset(block, "text", jstring(m->text));
		jappend(content, block);
		jset(msg, "content", content);
		jappend(msgs, msg);
	}
	jset(req, "messages", msgs);
	return req;
}

/*
 * Write all of buf to fd, handling short writes.
 * Returns 0 on success, -1 on error.
 */
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
 * Send request via webfs.
 * Clone a new connection, set URL and headers,
 * write the body, read the response.
 */
static char*
webfssend(Conv *c, char *body)
{
	int clonefd, fd, n;
	char buf[256], *data;
	char *clone, *ctl, *bodyf, *page;

	/* clone a connection */
	clone = "/mnt/web/clone";
	clonefd = open(clone, ORDWR);
	if(clonefd < 0){
		werrstr("open %s: %r", clone);
		return nil;
	}

	/* read the connection directory name */
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

	/* write URL and headers to ctl */
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

	/* write body, handling short writes */
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

	/* read response */
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

/*
 * Write a curl config file containing headers.
 * Keeps the API key out of the process argument list.
 * Returns the path on success (caller must free+remove), nil on error.
 */
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

/*
 * Fallback: use curl for plan9port environments
 * where webfs is not available.
 * Body is written to a temp file to avoid ARG_MAX limits.
 * Headers are passed via a config file to hide the API key from ps.
 */
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

	/* write body to temp file to avoid ARG_MAX limits */
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

/*
 * Send the conversation to Claude and return
 * the assistant's reply text.
 * Returns malloc'd string on success, nil on error
 * (with errstr set).
 */
char*
claudesend(Conv *c, Usage *usage)
{
	Json *req, *resp, *content, *block, *stopreason;
	Json *uobj;
	char *body, *data, *reply, *errtype, *errmsg;

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

	/* check for API error */
	errtype = jstr(resp, "type");
	if(errtype != nil && strcmp(errtype, "error") == 0){
		Json *errobj = jget(resp, "error");
		errmsg = jstr(errobj, "message");
		if(errmsg == nil)
			errmsg = "unknown API error";
		werrstr("API error: %s", errmsg);
		jsonfree(resp);
		return nil;
	}

	/* extract usage info if caller wants it */
	if(usage != nil){
		usage->input_tokens = 0;
		usage->output_tokens = 0;
		uobj = jget(resp, "usage");
		if(uobj != nil){
			usage->input_tokens = jint(uobj, "input_tokens");
			usage->output_tokens = jint(uobj, "output_tokens");
		}
	}

	/* check stop reason */
	stopreason = jget(resp, "stop_reason");
	if(stopreason != nil && stopreason->type == Jstring){
		if(usage != nil)
			usage->stop_reason = strdup(stopreason->str);
		if(strcmp(stopreason->str, "max_tokens") == 0)
			fprint(2, "[warning: response truncated at max_tokens]\n");
	} else {
		fprint(2, "[warning: no stop_reason in response]\n");
	}


	/* extract text from content[0].text */
	content = jget(resp, "content");
	if(content == nil || content->type != Jarray || content->nitem == 0){
		jsonfree(resp);
		werrstr("no content in response");
		return nil;
	}
	block = jidx(content, 0);
	if(block == nil){
		jsonfree(resp);
		werrstr("empty content array");
		return nil;
	}
	reply = jstr(block, "text");
	if(reply == nil){
		jsonfree(resp);
		werrstr("no text in content block");
		return nil;
	}
	reply = strdup(reply);
	jsonfree(resp);
	return reply;
}

/*
 * Fetch the list of available models from the API.
 * GET /v1/models?limit=100
 *
 * Returns a malloc'd array of ModelInfo structs via *out,
 * and the count via return value.  Returns -1 on error.
 * Caller must free each id/name string and the array.
 */
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

	/* check for API error */
	errtype = jstr(resp, "type");
	if(errtype != nil && strcmp(errtype, "error") == 0){
		Json *errobj = jget(resp, "error");
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
		/* Parse max_output_tokens, default to 4096 if not present */
		list[i].max_output_tokens = jint(item, "max_output_tokens");
		if(list[i].max_output_tokens <= 0)
			list[i].max_output_tokens = 4096;
	}

	jsonfree(resp);
	*out = list;
	return n;
}
