#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"

static char *apiurl = "https://api.anthropic.com/v1/messages";
static char *apiversion = "2023-06-01";

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
	Json *req, *msgs, *msg, *content;
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
		{
			Json *block;
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

/*
 * Send request via webfs.
 * Clone a new connection, set URL and headers,
 * write the body, read the response.
 */
static char*
webfssend(Conv *c, char *body)
{
	int fd, n, tot;
	char buf[256], *data;
	char *clone, *ctl, *bodyf, *page;

	/* clone a connection */
	clone = "/mnt/web/clone";
	fd = open(clone, ORDWR);
	if(fd < 0)
		return smprint("open %s: %r", clone);

	/* read the connection directory name */
	n = read(fd, buf, sizeof buf - 1);
	if(n <= 0){
		close(fd);
		return smprint("read clone: %r");
	}
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';
	close(fd);

	free(c->webdir);
	c->webdir = smprint("/mnt/web/%s", buf);

	/* write URL to ctl */
	ctl = smprint("%s/ctl", c->webdir);
	fd = open(ctl, OWRITE);
	free(ctl);
	if(fd < 0)
		return smprint("open ctl: %r");

	fprint(fd, "url %s\n", apiurl);
	fprint(fd, "request POST\n");
	fprint(fd, "headers Content-Type: application/json\r\n");
	fprint(fd, "headers x-api-key: %s\r\n", c->apikey);
	fprint(fd, "headers anthropic-version: %s\r\n", apiversion);
	close(fd);

	/* write body */
	bodyf = smprint("%s/postbody", c->webdir);
	fd = open(bodyf, OWRITE);
	free(bodyf);
	if(fd < 0)
		return smprint("open postbody: %r");
	if(write(fd, body, strlen(body)) != strlen(body)){
		close(fd);
		return smprint("write postbody: %r");
	}
	close(fd);

	/* read response */
	page = smprint("%s/body", c->webdir);
	fd = open(page, OREAD);
	free(page);
	if(fd < 0)
		return smprint("open body: %r");

	tot = 0;
	n = 0;
	data = nil;
	for(;;){
		data = realloc(data, tot + 8192 + 1);
		if(data == nil)
			sysfatal("realloc: %r");
		n = read(fd, data + tot, 8192);
		if(n <= 0)
			break;
		tot += n;
	}
	close(fd);
	if(tot == 0){
		free(data);
		return smprint("empty response from API");
	}
	data[tot] = '\0';
	return data;
}

/*
 * Fallback: use curl for plan9port environments
 * where webfs is not available.
 */
static char*
curlsend(Conv *c, char *body)
{
	int pfd[2], n, tot;
	char *data;

	if(pipe(pfd) < 0)
		return smprint("pipe: %r");

	switch(fork()){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		return smprint("fork: %r");
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		close(pfd[1]);
		execl("/usr/bin/curl", "curl", "-s",
			"-X", "POST",
			"-H", "Content-Type: application/json",
			"-H", smprint("x-api-key: %s", c->apikey),
			"-H", smprint("anthropic-version: %s", apiversion),
			"-d", body,
			apiurl,
			nil);
		sysfatal("exec curl: %r");
	}
	close(pfd[1]);

	tot = 0;
	data = nil;
	for(;;){
		data = realloc(data, tot + 8192 + 1);
		if(data == nil)
			sysfatal("realloc: %r");
		n = read(pfd[0], data + tot, 8192);
		if(n <= 0)
			break;
		tot += n;
	}
	close(pfd[0]);
	waitpid();

	if(tot == 0){
		free(data);
		return smprint("empty response from API");
	}
	data[tot] = '\0';
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
claudesend(Conv *c)
{
	Json *req, *resp, *content, *block;
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
		werrstr("no response");
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
		reply = smprint("API error: %s", errmsg);
		jsonfree(resp);
		werrstr("%s", reply);
		free(reply);
		return nil;
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
