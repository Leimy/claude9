#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include "json.h"
#include "claude.h"
#include "claudeimpl.h"

static char *apiversion = "2023-06-01";	/* anthropic-version header */

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
 * ToolCall, Tooldef, Reply, and Provider are defined in
 * claudeimpl.h, shared with the provider implementations
 * (openai.c); the openai* entry points are declared there
 * too.
 */
static int anthropicheaders(int, Conv*);
static Json* anthropicbuildreq(Conv*);
static Reply* anthropicreadstream(Conv*, Biobuf*, Usage*,
	void (*)(char*, void*), void*);

static Provider providers[] = {
	{ "anthropic",
	  "https://api.anthropic.com/v1/messages",
	  "https://api.anthropic.com/v1/models?limit=100",
	  anthropicheaders,
	  anthropicbuildreq,
	  anthropicreadstream,
	  nil },
	{ "openai",
	  "https://api.openai.com/v1/chat/completions",
	  "https://api.openai.com/v1/models",
	  openaiheaders,
	  openaibuildreq,
	  openaireadstream,
	  openaiquirk },
	/*
	 * Same models, same key, different wire format: the
	 * Responses API, which OpenAI's newer reasoning models
	 * require for function tools (see responses.c).  No models
	 * endpoint of its own -- the openai entry already lists
	 * them (providerhasmodels).
	 */
	{ "responses",
	  "https://api.openai.com/v1/responses",
	  nil,
	  openaiheaders,
	  responsesbuildreq,
	  responsesreadstream,
	  responsesquirk },
};

int
providerlookup(char *name)
{
	int i;

	if(name == nil)
		return -1;
	for(i = 0; i < nelem(providers); i++)
		if(strcmp(name, providers[i].name) == 0)
			return i;
	return -1;
}

char*
providername(int prov)
{
	if(prov < 0 || prov >= nelem(providers))
		return "unknown";
	return providers[prov].name;
}

int
providercount(void)
{
	return nelem(providers);
}

int
providerhasmodels(int prov)
{
	if(prov < 0 || prov >= nelem(providers))
		return 0;
	return providers[prov].modelsurl != nil;
}

/*
 * Resolve a conversation's provider index to the vtable,
 * falling back to the first (anthropic) entry if the index is
 * somehow out of range -- a wrong-but-working request beats a
 * nil dereference.
 */
static Provider*
provof(Conv *c)
{
	if(c->prov < 0 || c->prov >= nelem(providers))
		return &providers[0];
	return &providers[c->prov];
}

/*
 * Single source of truth for the tools we expose to the model.
 *
 * Each tool takes up to Maxargs string parameters, all
 * required; params[0] is always the path.  The same table
 * drives the JSON schema (toolschema/mktools) and argument
 * extraction (parseinput); ToolCall.args holds the values in
 * params order.  findtool() maps API names to the Acreate/...
 * enum.  Providers outside this file reach the table through
 * tooldef(i) (see claudeimpl.h for why it is not extern data).
 */
static Tooldef tools[] = {
	{ Acreate, "create_file",
		"Create or overwrite a file with the given contents. "
		"Parent directories are created automatically.",
		{{ "path", "File path to create" },
		 { "contents", "Complete file contents" }}},

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
		{{ "path", "File path to edit" },
		 { "old_str", "The exact text to search for in the file. "
			"Must match exactly once." },
		 { "new_str", "The replacement text. Use empty string to delete." }}},

	{ Adelete, "delete_file",
		"Delete a file.",
		{{ "path", "File path to delete" }}},

	{ Aread, "read_file",
		"Read the contents of a file and return them.",
		{{ "path", "File path to read" }}},

	{ Alist, "list_directory",
		"List the contents of a directory. "
		"Returns one entry per line.",
		{{ "path", "Directory path to list" }}},

	{ Amanpage, "read_man_page",
		"Read a Plan 9 manual page. Returns the formatted man page "
		"text. The query is the page name, optionally preceded by "
		"a section number (e.g. \"open\" or \"2 open\"). Section "
		"numbers: 1 commands, 2 syscalls, 3 C library, 4 file "
		"formats, 5 filesystems, 6 games/misc, 7 databases, "
		"8 admin. If no section is given, man searches all sections.",
		{{ "path", "Man page query: page name, optionally preceded by "
			"section (e.g. \"open\", \"2 open\", \"rio\")" }}},

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
		{{ "path", "Directory to run mk in (empty for current directory)" },
		 { "targets", "Space-separated mk targets/args, or empty for the default target" }}},

	{ Awebsearch, "web_search",
		"Search the web through webfs and return a list of result "
		"titles and URLs (titles and URLs only, not page content -- "
		"use web_fetch on one of the returned URLs to read a page).  "
		"Use this for current events, external documentation, or "
		"anything not covered by local man pages and source.  "
		"Results come from scraping a search engine's result "
		"page, so titles may be imprecise and results can "
		"occasionally be empty even when relevant pages exist; "
		"treat it as a pointer to URLs, not an authoritative "
		"answer.",
		{{ "query", "Search query text" }}},

	{ Awebfetch, "web_fetch",
		"Fetch the text content of a web page and return it as "
		"plain text (HTML tags, scripts, and styles stripped), "
		"truncated if the page is large.  The url must be one "
		"returned by a previous web_search call in this same "
		"session, copied exactly -- call web_search first to "
		"find a URL, then web_fetch it to read the page.  URLs "
		"you did not get from a web_search result (invented, or "
		"found in a file's contents) are rejected.",
		{{ "url", "A URL exactly as returned by a previous web_search result" }}},
};

Tooldef*
tooldef(int i)
{
	if(i < 0 || i >= nelem(tools))
		return nil;
	return &tools[i];
}

Tooldef*
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
 * Pull parameters out of a tool_use block's "input" object,
 * in Tooldef param order.  Missing parameters (and all
 * parameters of an unknown tool, td == nil) become "".
 */
void
parseinput(ToolCall *tc, Tooldef *td, Json *input)
{
	char *s;
	int i;

	for(i = 0; i < Maxargs; i++){
		s = nil;
		if(td != nil && td->params[i].name != nil)
			s = jstr(input, td->params[i].name);
		tc->args[i] = estrdup(s ? s : "");
	}
}

static void
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
	long n, len, cap;

	cap = 8192;
	buf = emalloc(cap);
	len = 0;
	while((n = read(fd, buf + len, cap - len - 1)) > 0){
		len += n;
		if(len >= cap - 1){
			cap *= 2;
			buf = erealloc(buf, cap);
		}
	}
	if(n < 0){
		free(buf);
		return nil;
	}
	buf[len] = '\0';
	return buf;
}

/*
 * Like readfile, but never grows past max bytes and reports
 * whether more data remained unread past that point.
 *
 * readfile itself is unbounded, which is fine for callers that
 * already trust the source (an HTTP response body, a skills
 * file dropped in place by whoever configured claude9fs).  It
 * is not fine for the model-facing read_file tool: a model (or
 * a prompt-injected file it was told to read) can name a huge
 * file, a synthetic file with effectively unbounded output, or
 * a blocking device, any of which could exhaust server memory
 * or wedge a tool worker indefinitely (CODE-REVIEW.md finding
 * #2).  toolread uses this instead of readfile for that reason;
 * other readfile callers are unaffected.
 */
static char*
readfilelimit(int fd, long max, int *truncatedp)
{
	char *buf;
	long n, len, cap;

	*truncatedp = 0;
	cap = 8192;
	if(cap > max + 1)
		cap = max + 1;
	buf = emalloc(cap);
	len = 0;
	n = 0;
	for(;;){
		/*
		 * Stop as soon as the cap is full.  Do not peek one
		 * more byte: on a stream or synthetic file that extra
		 * read can block indefinitely after we already have all
		 * the output we are willing to return.  "truncated"
		 * therefore means "the limit was reached; more data may
		 * exist", which is the only safe claim for a general 9P
		 * file without a trustworthy length.
		 */
		if(len >= max){
			*truncatedp = 1;
			break;
		}
		n = read(fd, buf + len, cap - len - 1);
		if(n <= 0)
			break;
		len += n;
		if(len >= cap - 1 && len < max){
			cap *= 2;
			if(cap > max + 1)
				cap = max + 1;
			buf = erealloc(buf, cap);
		}
	}
	if(n < 0){
		free(buf);
		return nil;
	}
	buf[len] = '\0';
	return buf;
}

static char *defaultsysprompt =
	"You are a coding assistant running on Plan 9 (9front). "
	"You can use all tools supplied with each request.  In particular, "
	"read_file reads file contents and list_directory browses directories; "
	"use them when the user asks you to inspect or review a project.  "
	"You also have tools to create, edit, and delete files. "
	"Use the tools when the user asks you to inspect or make changes. "
	"Do not claim that a supplied tool is unavailable after you have used it. "
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

/*
 * See the doc comment in claude.h.  Used by convnew, by
 * wrsystem (a system-file write changes the base but should
 * not lose whatever skills are currently in effect), and by
 * a skills reload (the base is unchanged; only the skills
 * argument is new).
 */
void
convsetprompt(Conv *c, char *base, char *skills)
{
	char *oldbase, *oldsys;

	oldbase = c->basesys;
	oldsys = c->sysprompt;
	c->basesys = estrdup(base ? base : "");
	if(skills != nil && skills[0] != '\0')
		c->sysprompt = esmprint("%s%s", c->basesys, skills);
	else
		c->sysprompt = estrdup(c->basesys);
	free(oldbase);
	free(oldsys);
}

Conv*
convnew(char *apikey, char *model, int maxtokens, char *sysprompt, char *skills)
{
	Conv *c;

	c = emallocz(sizeof *c, 1);
	c->prov = providerlookup("anthropic");
	c->apikey = estrdup(apikey);
	c->model = estrdup(model);
	c->maxtokens = maxtokens;
	convsetprompt(c, sysprompt != nil ? sysprompt : defaultsysprompt, skills);
	return c;
}

/*
 * Free all messages, leaving the conversation empty but
 * otherwise configured (model, tokens, thinking, system).
 */
static void
clearsearchurls(Conv *c)
{
	int i;

	for(i = 0; i < c->nsearchurls; i++)
		free(c->searchurls[i]);
	free(c->searchurls);
	c->searchurls = nil;
	c->nsearchurls = 0;
}

void
convclear(Conv *c)
{
	Msg *m, *next;

	for(m = c->msgs; m != nil; m = next){
		next = m->next;
		free(m->text);
		free(m->rawjson);
		free(m);
	}
	c->msgs = nil;
	c->tail = nil;
	/* Search-derived fetch authority is conversation history too. */
	clearsearchurls(c);
}

void
convfree(Conv *c)
{
	if(c == nil)
		return;
	convclear(c);
	free(c->apikey);
	free(c->model);
	free(c->baseurl);
	free(c->effort);
	free(c->advisormodel);
	free(c->advisorcache);
	free(c->basesys);
	free(c->sysprompt);
	free(c);
}

Msg*
msgnew(int role, char *text, char *rawjson)
{
	Msg *m;

	m = emallocz(sizeof *m, 1);
	m->role = role;
	m->text = estrdup(text != nil ? text : "");
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
 * A message begins a new "exchange" iff it is a real user
 * turn: role Muser with no rawjson.  Tool_results messages
 * (Muser with rawjson) and assistant messages do not; they
 * belong to the exchange opened by the preceding real user
 * turn.  See convcompact.
 */
static int
exchangestart(Msg *m)
{
	return m->role == Muser && m->rawjson == nil;
}

int
convnexchanges(Conv *c)
{
	Msg *m;
	int n;

	n = 0;
	for(m = c->msgs; m != nil; m = m->next)
		if(exchangestart(m))
			n++;
	return n;
}

long
convinputbytes(Conv *c)
{
	Msg *m;
	long n;

	n = 0;
	for(m = c->msgs; m != nil; m = m->next){
		if(m->text != nil)
			n += strlen(m->text);
		if(m->rawjson != nil)
			n += strlen(m->rawjson);
	}
	return n;
}

int
convcompact(Conv *c, int keep)
{
	Msg *m, *next, *cut;
	int total, todrop, dropped;

	if(keep < 1)
		keep = 1;
	total = convnexchanges(c);
	/*
	 * Keep at least the most recent keep exchanges.  If the
	 * history is already that short, nothing is safe to drop.
	 */
	if(total <= keep)
		return 0;
	todrop = total - keep;

	/*
	 * Walk forward counting exchange starts.  cut is the
	 * first message we keep: the (todrop+1)-th exchange start.
	 * Everything before cut -- whole exchanges, so every
	 * tool_use keeps its tool_result -- is freed.  cut is a
	 * real user turn, so the surviving list starts on a user
	 * message as the API requires.
	 */
	cut = nil;
	dropped = 0;
	for(m = c->msgs; m != nil; m = m->next){
		if(exchangestart(m)){
			if(dropped == todrop){
				cut = m;
				break;
			}
			dropped++;
		}
	}
	if(cut == nil || cut == c->msgs)
		return 0;	/* nothing to do (shouldn't happen) */

	dropped = 0;
	for(m = c->msgs; m != cut; m = next){
		next = m->next;
		free(m->text);
		free(m->rawjson);
		free(m);
		dropped++;
	}
	c->msgs = cut;
	/* tail is unchanged: we only removed from the front */
	return dropped;
}

/*
 * JSON schema object for one tool's parameters: the payload
 * both providers advertise, under different wrappers
 * (anthropic input_schema, openai function.parameters).
 */
Json*
toolschema(Tooldef *td)
{
	Json *input, *props, *p, *req;
	Toolparam *tp;

	input = jobject();
	jset(input, "type", jstring("object"));
	props = jobject();
	req = jarray();
	for(tp = td->params; tp < td->params + Maxargs && tp->name != nil; tp++){
		p = jobject();
		jset(p, "type", jstring("string"));
		jset(p, "description", jstring(tp->desc));
		jset(props, tp->name, p);
		jappend(req, jstring(tp->name));
	}
	jset(input, "properties", props);
	jset(input, "required", req);
	return input;
}

/*
 * Build tool definitions JSON, anthropic shape.
 */
static Json*
mktools(Conv *c)
{
	Json *arr, *t, *cc, *cache;
	Tooldef *td;
	int i;

	arr = jarray();
	for(i = 0; i < nelem(tools); i++){
		td = &tools[i];
		t = jobject();
		jset(t, "name", jstring(td->name));
		jset(t, "description", jstring(td->desc));
		jset(t, "input_schema", toolschema(td));
		jappend(arr, t);
	}
	if(c->advisormodel != nil){
		t = jobject();
		jset(t, "type", jstring("advisor_20260301"));
		jset(t, "name", jstring("advisor"));
		jset(t, "model", jstring(c->advisormodel));
		if(c->advisormaxuses > 0)
			jset(t, "max_uses", jintval(c->advisormaxuses));
		if(c->advisormaxtokens > 0)
			jset(t, "max_tokens", jintval(c->advisormaxtokens));
		if(c->advisorcache != nil){
			cache = jobject();
			jset(cache, "type", jstring("ephemeral"));
			jset(cache, "ttl", jstring(c->advisorcache));
			jset(t, "caching", cache);
		}
		jappend(arr, t);
	}
	/* Cache all stable tool definitions, including advisor. */
	if(arr->nitem > 0){
		t = jidx(arr, arr->nitem - 1);
		cc = jobject();
		jset(cc, "type", jstring("ephemeral"));
		jset(t, "cache_control", cc);
	}
	return arr;
}

/*
 * True if s is nil, empty, or contains only whitespace.
 * The API rejects text content blocks that are empty OR
 * whitespace-only ("text content blocks must contain
 * non-whitespace text"), so both cases must be treated
 * identically everywhere a text block is emitted.
 */
int
blankstr(char *s)
{
	if(s == nil)
		return 1;
	for(; *s != '\0'; s++)
		if(*s != ' ' && *s != '\t' && *s != '\n'
		&& *s != '\r' && *s != '\v' && *s != '\f')
			return 0;
	return 1;
}

/*
 * Remove text content blocks that are empty or whitespace-only
 * from a parsed content array, in place.  Tool_use, tool_result,
 * thinking and non-blank text blocks are kept.  This repairs a
 * rawjson snapshot replayed from an earlier round (or a wedged
 * conversation) before it is sent: the API rejects any text
 * block without non-whitespace text, and a single bad block
 * makes every resend fail.  Returns the number of blocks kept.
 */
static int
striptextblocks(Json *content)
{
	Json *block;
	char *btype;
	int i, keep;

	if(content == nil || content->type != Jarray)
		return content != nil ? content->nitem : 0;
	keep = 0;
	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype != nil && strcmp(btype, "text") == 0
		&& blankstr(jstr(block, "text"))){
			jsonfree(block);
			continue;	/* drop this blank text block */
		}
		content->items[keep++] = block;
	}
	content->nitem = keep;
	return keep;
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

/*
 * Collect all tool_use IDs from a content array.
 * Returns a malloc'd array of estrdup'd ID strings;
 * sets *np to the count.  Caller frees both the strings
 * and the array.
 */
static char**
collecttoolids(Json *content, int *np)
{
	Json *block;
	char *btype, *id;
	int i, n, cap;
	char **ids;

	*np = 0;
	if(content == nil || content->type != Jarray)
		return nil;
	cap = 0;
	n = 0;
	ids = nil;
	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil || strcmp(btype, "tool_use") != 0)
			continue;
		id = jstr(block, "id");
		if(id == nil || id[0] == '\0')
			continue;
		if(n >= cap){
			cap = cap ? cap * 2 : 8;
			ids = erealloc(ids, cap * sizeof(char*));
		}
		ids[n++] = estrdup(id);
	}
	*np = n;
	return ids;
}

/*
 * Check whether a content array contains a tool_result
 * block with the given tool_use_id.
 */
static int
hastoolresult(Json *content, char *id)
{
	Json *block;
	char *btype, *rid;
	int i;

	if(content == nil || content->type != Jarray)
		return 0;
	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil || strcmp(btype, "tool_result") != 0)
			continue;
		rid = jstr(block, "tool_use_id");
		if(rid != nil && strcmp(rid, id) == 0)
			return 1;
	}
	return 0;
}

/*
 * Repair orphaned tool_use blocks in the messages array.
 *
 * The API requires that every tool_use block in an assistant
 * message has a matching tool_result (same tool_use_id) in
 * the immediately following user message.  If the conversation
 * gets into a state where this invariant is violated (e.g.
 * due to a dropped connection, a bug in an earlier build, or
 * an interrupted tool loop), every subsequent API call will
 * fail and the conversation is permanently wedged.
 *
 * This function scans the assembled messages array and injects
 * synthetic tool_result blocks into the next user message for
 * any orphaned tool_use IDs.  If the next message is not a
 * user message (or doesn't exist), one is inserted.
 */
static void
repairtooluse(Json *msgs)
{
	Json *msg, *content, *ncontent, *block, *newmsg;
	char *role, **ids;
	int i, j, nids, ninj, repaired;

	if(msgs == nil || msgs->type != Jarray)
		return;

	repaired = 0;
	for(i = 0; i < msgs->nitem; i++){
		msg = msgs->items[i];
		role = jstr(msg, "role");
		if(role == nil || strcmp(role, "assistant") != 0)
			continue;
		content = jget(msg, "content");
		ids = collecttoolids(content, &nids);
		if(nids == 0){
			free(ids);
			continue;
		}

		/*
		 * Find the next message; it should be a user
		 * message containing matching tool_results.
		 */
		ncontent = nil;
		if(i + 1 < msgs->nitem){
			role = jstr(msgs->items[i + 1], "role");
			if(role != nil && strcmp(role, "user") == 0){
				ncontent = jget(msgs->items[i + 1], "content");
				if(ncontent != nil && ncontent->type != Jarray)
					ncontent = nil;
			}
		}

		ninj = 0;
		for(j = 0; j < nids; j++){
			if(ncontent != nil && hastoolresult(ncontent, ids[j]))
				continue;
			/*
			 * Orphaned tool_use: no matching tool_result.
			 * Inject one into the next user message's
			 * content array.  If there is no next user
			 * message, create one.
			 */
			if(ncontent == nil){
				newmsg = jobject();
				jset(newmsg, "role", jstring("user"));
				ncontent = jarray();
				jset(newmsg, "content", ncontent);
				jinsert(msgs, i + 1, newmsg);
			}
			block = jobject();
			jset(block, "type", jstring("tool_result"));
			jset(block, "tool_use_id", jstring(ids[j]));
			jset(block, "content",
				jstring("error: tool result lost (session recovery)"));
			/*
			 * tool_result blocks must precede any other
			 * content in the user message, so insert at
			 * the front (after any already injected).
			 */
			jinsert(ncontent, ninj, block);
			ninj++;
			repaired++;
		}

		for(j = 0; j < nids; j++)
			free(ids[j]);
		free(ids);
	}
	if(repaired > 0)
		fprint(2, "claude: repaired %d orphaned tool_use block%s\n",
			repaired, repaired > 1 ? "s" : "");
}

/*
 * Check whether a content array contains a tool_use block
 * with the given id.
 */
static int
hastooluse(Json *content, char *id)
{
	Json *block;
	char *btype, *bid;
	int i;

	if(content == nil || content->type != Jarray)
		return 0;
	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil || strcmp(btype, "tool_use") != 0)
			continue;
		bid = jstr(block, "id");
		if(bid != nil && strcmp(bid, id) == 0)
			return 1;
	}
	return 0;
}

/*
 * Repair orphaned tool_result blocks: the mirror image of
 * repairtooluse.  Every tool_result in a user message must
 * reference a tool_use in the immediately preceding assistant
 * message; if that assistant turn was lost or mangled (e.g. a
 * corrupt rawjson snapshot that failed to reparse and was
 * replaced by placeholder text), the leftover tool_results
 * make the API reject every subsequent request.  Drop them.
 */
static void
repairtoolresults(Json *msgs)
{
	Json *msg, *content, *pcontent, *block;
	char *role, *btype, *rid;
	int i, j, keep, dropped;

	if(msgs == nil || msgs->type != Jarray)
		return;
	dropped = 0;
	for(i = 0; i < msgs->nitem; i++){
		msg = msgs->items[i];
		role = jstr(msg, "role");
		if(role == nil || strcmp(role, "user") != 0)
			continue;
		content = jget(msg, "content");
		if(content == nil || content->type != Jarray)
			continue;
		pcontent = nil;
		if(i > 0){
			role = jstr(msgs->items[i - 1], "role");
			if(role != nil && strcmp(role, "assistant") == 0)
				pcontent = jget(msgs->items[i - 1], "content");
		}
		keep = 0;
		for(j = 0; j < content->nitem; j++){
			block = content->items[j];
			btype = jstr(block, "type");
			if(btype != nil && strcmp(btype, "tool_result") == 0){
				rid = jstr(block, "tool_use_id");
				if(rid == nil || !hastooluse(pcontent, rid)){
					jsonfree(block);
					dropped++;
					continue;
				}
			}
			content->items[keep++] = block;
		}
		content->nitem = keep;
		/*
		 * The API rejects empty content arrays; if the
		 * message consisted only of dropped tool_results,
		 * leave a placeholder behind.
		 */
		if(keep == 0){
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text",
				jstring("(tool results dropped: session recovery)"));
			jappend(content, block);
		}
	}
	if(dropped > 0)
		fprint(2, "claude: dropped %d orphaned tool_result block%s\n",
			dropped, dropped > 1 ? "s" : "");
}

/*
 * Build the neutral messages array from a Conv's message list:
 * one {role, content} object per surviving message, content
 * always a Jarray of blocks (text/thinking/tool_use/
 * tool_result), with blank text blocks stripped, consecutive
 * same-role messages merged, and the tool_use/tool_result
 * protocol invariants repaired.
 *
 * Both provider builders call this -- anthropicbuildreq uses
 * it directly as the wire "messages" array; openaibuildreq
 * translates each {role, content} entry to the OpenAI shape
 * (appendassistantmsg/appendtoolresultmsgs) -- so a corrupted
 * or interrupted history is normalized once, before either
 * wire-format translation, instead of only on the Anthropic
 * path (see PROVIDERS.md and CODE-REVIEW.md finding #5).
 */
Json*
neutralmessages(Conv *c)
{
	Json *msgs, *msg, *content;
	Msg *m;

	msgs = jarray();
	for(m = c->msgs; m != nil; m = m->next){
		char *role, *prole;
		Json *pcontent;
		int i;

		role = m->role == Muser ? "user" : "assistant";
		content = nil;
		if(m->rawjson != nil){
			content = jsonparse(m->rawjson);
			/*
			 * A reparse failure here is serious: the
			 * snapshot carries tool_use/tool_result
			 * blocks, and falling back to plain text
			 * silently breaks the tool protocol.  The
			 * repair passes below recover the protocol,
			 * but say what happened.
			 */
			if(content == nil)
				fprint(2, "claude: stored message failed to reparse: %r\n");
		}
		/*
		 * Repair the replayed content array: strip any
		 * empty/whitespace-only text blocks the API would
		 * reject.  This also recovers a conversation that
		 * was wedged by an earlier build of this program.
		 */
		if(content != nil)
			striptextblocks(content);
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
		/*
		 * Merge consecutive messages with the same role into
		 * one message.  These arise naturally: a failed or
		 * round-capped tool loop leaves the conversation
		 * ending with a user message of tool_results, and the
		 * next prompt appends another user message.  Merging
		 * keeps the tool_result blocks in the message that
		 * immediately follows the tool_use (and first within
		 * it), which is what the API requires.
		 */
		if(msgs->nitem > 0){
			msg = msgs->items[msgs->nitem - 1];
			prole = jstr(msg, "role");
			if(prole != nil && strcmp(prole, role) == 0){
				if(content == nil && !blankstr(m->text))
					content = mktextcontent(m->text);
				if(content != nil){
					pcontent = jget(msg, "content");
					for(i = 0; i < content->nitem; i++)
						jappend(pcontent, content->items[i]);
					content->nitem = 0;
					jsonfree(content);
				}
				continue;
			}
		}
		if(content == nil){
			/*
			 * Guard against empty/whitespace-only text
			 * content blocks: the API rejects both
			 * {"type":"text","text":""} and a block whose
			 * text is only whitespace.  Use a harmless
			 * placeholder when the message text is blank.
			 */
			if(blankstr(m->text))
				content = mktextcontent("(no text)");
			else
				content = mktextcontent(m->text);
		}
		msg = jobject();
		jset(msg, "role", jstring(role));
		jset(msg, "content", content);
		jappend(msgs, msg);
	}

	/*
	 * Repair any orphaned tool_use and tool_result blocks
	 * before sending.  This recovers conversations wedged by
	 * earlier bugs, dropped connections, or interrupted tool
	 * loops.
	 */
	repairtooluse(msgs);
	repairtoolresults(msgs);

	return msgs;
}

/*
 * Remove what the Anthropic API would reject from a neutral
 * messages array produced (in part) by the OpenAI Responses
 * provider: "reasoning" blocks (Responses reasoning items, the
 * counterpart of our thinking blocks, meaningless here) and the
 * "item_id" field Responses text/tool_use blocks carry (an
 * unknown field in an Anthropic content block is an error).
 * See the neutral-form note in claudeimpl.h.  This is the
 * mirror image of openai.c/responses.c skipping thinking blocks
 * on their side, so a conversation can move between providers.
 *
 * An assistant turn cannot consist of reasoning alone (the
 * model always emits a message or a call after it), but a
 * corrupt snapshot might; the API rejects an empty content
 * array, so leave a placeholder in that case.
 */
static void
stripforeign(Json *msgs)
{
	Json *msg, *content, *block;
	char *btype;
	int i, j, keep;

	if(msgs == nil || msgs->type != Jarray)
		return;
	for(i = 0; i < msgs->nitem; i++){
		msg = msgs->items[i];
		content = jget(msg, "content");
		if(content == nil || content->type != Jarray)
			continue;
		keep = 0;
		for(j = 0; j < content->nitem; j++){
			block = content->items[j];
			btype = jstr(block, "type");
			if(btype != nil && strcmp(btype, "reasoning") == 0){
				jsonfree(block);
				continue;
			}
			jdel(block, "item_id");
			content->items[keep++] = block;
		}
		content->nitem = keep;
		if(keep == 0){
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text", jstring("(no text)"));
			jappend(content, block);
		}
	}
}

static Json*
anthropicbuildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *block, *sys, *cc;

	req = jobject();
	jset(req, "model", jstring(c->model));
	jset(req, "max_tokens", jintval(c->maxtokens));

	/*
	 * Extended thinking.  Two API shapes, model-dependent:
	 *
	 * Thinkbudget (opus/sonnet/haiku families):
	 *   thinking: {type: "enabled", budget_tokens: N}
	 * Whoever sets Conv.thinking enforces the API's invariant
	 * 1024 <= budget < maxtokens (see wrthinking in claude9fs.c),
	 * so the value is passed through as-is.
	 *
	 * Thinkadaptive (fable family):
	 *   thinking: {type: "adaptive"}
	 *   output_config: {effort: "..."}   (optional)
	 * These models reject type "enabled" outright.
	 */
	if(c->thinkmode == Thinkbudget && c->thinking > 0){
		Json *think;

		think = jobject();
		jset(think, "type", jstring("enabled"));
		jset(think, "budget_tokens", jintval(c->thinking));
		jset(req, "thinking", think);
	} else if(c->thinkmode == Thinkadaptive){
		Json *think, *oc;

		think = jobject();
		jset(think, "type", jstring("adaptive"));
		jset(req, "thinking", think);
		if(c->effort != nil && c->effort[0] != '\0'){
			oc = jobject();
			jset(oc, "effort", jstring(c->effort));
			jset(req, "output_config", oc);
		}
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

	jset(req, "tools", mktools(c));

	msgs = neutralmessages(c);
	stripforeign(msgs);

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
 * Opening the response body failed: pull the HTTP error body
 * from webfs, which for the Anthropic API is JSON with a
 * detailed message, and put it in errstr.
 */
static void
weberror(char *webdir)
{
	char *path, *ebody, *emsg, orig[ERRMAX];
	Json *ej;
	int fd;

	/*
	 * Remember why the body open failed: the errorbody open and
	 * read below overwrite errstr, and a connection that never got
	 * an HTTP reply (refused, unreachable, wrong port) has an empty
	 * errorbody, which must not replace the real reason.
	 */
	rerrstr(orig, sizeof orig);
	path = esmprint("%s/errorbody", webdir);
	fd = open(path, OREAD);
	free(path);
	if(fd < 0){
		werrstr("%s", orig);
		return;	/* keep errstr from the failed body open */
	}
	ebody = readfile(fd);
	close(fd);
	if(ebody == nil || ebody[0] == '\0'){
		free(ebody);
		werrstr("%s", orig);
		return;
	}
	ej = jsonparse(ebody);
	emsg = jstr(jget(ej, "error"), "message");
	if(emsg != nil)
		werrstr("API error: %s", emsg);
	else
		werrstr("API error: %.256s", ebody);
	jsonfree(ej);
	free(ebody);
}

/* Anthropic auth: api key plus a pinned API version. */
static int
anthropicheaders(int fd, Conv *c)
{
	if(fprint(fd, "headers x-api-key: %s\r\n", c->apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0
	|| (c->advisormodel != nil
	 && fprint(fd, "headers anthropic-beta: advisor-tool-2026-03-01\r\n") < 0))
		return -1;
	return 0;
}

/*
 * Perform an HTTP request to an API through webfs; the
 * provider supplies the auth headers.  postbody nil means GET.
 * On success returns an open fd for the response body and
 * stores the connection fd in *clonefdp (caller closes both);
 * on error returns -1 with errstr set.
 */
static int
webhttp(Provider *p, Conv *c, char *url, char *postbody, int stream, int *clonefdp)
{
	int clonefd, fd, n;
	char buf[256], *webdir, *path;

	clonefd = open("/mnt/web/clone", ORDWR);
	if(clonefd < 0)
		return -1;
	n = read(clonefd, buf, sizeof buf - 1);
	if(n <= 0){
		werrstr("read web clone: %r");
		close(clonefd);
		return -1;
	}
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';
	webdir = esmprint("/mnt/web/%s", buf);

	path = esmprint("%s/ctl", webdir);
	fd = open(path, OWRITE);
	free(path);
	if(fd < 0)
		goto err;
	if(fprint(fd, "url %s\n", url) < 0
	|| fprint(fd, "request %s\n", postbody ? "POST" : "GET") < 0
	|| (postbody && fprint(fd, "headers Content-Type: application/json\r\n") < 0)
	|| (stream && fprint(fd, "headers Accept: text/event-stream\r\n") < 0)
	|| p->headers(fd, c) < 0){
		close(fd);
		goto err;
	}
	close(fd);

	if(postbody != nil){
		path = esmprint("%s/postbody", webdir);
		fd = open(path, OWRITE);
		free(path);
		if(fd < 0)
			goto err;
		if(writeall(fd, postbody, strlen(postbody)) < 0){
			close(fd);
			goto err;
		}
		close(fd);
	}

	path = esmprint("%s/body", webdir);
	fd = open(path, OREAD);
	free(path);
	if(fd < 0){
		/*
		 * Opening body blocks until the response headers
		 * arrive, so a cancel usually lands right here.  The
		 * request is still in flight, and errorbody would
		 * block until it finishes: skip it, and let the
		 * caller report the cancel.
		 */
		if(!cancelled(c))
			weberror(webdir);
		goto err;
	}
	free(webdir);
	*clonefdp = clonefd;
	return fd;

err:
	free(webdir);
	close(clonefd);
	return -1;
}

void
toolfree(ToolCall *t)
{
	ToolCall *next;
	int i;

	while(t != nil){
		next = t->next;
		free(t->id);
		free(t->name);
		for(i = 0; i < Maxargs; i++)
			free(t->args[i]);
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

enum {
	Toolreadmax = 262144,	/* cap on read_file tool output; see readfilelimit */
};

static char*
toolread(char *path)
{
	int fd, truncated;
	char *data, *out;
	Dir *d;

	fd = open(path, OREAD);
	if(fd < 0)
		return esmprint("error: open %s: %r", path);

	/*
	 * Directories read back as raw 9P directory-entry bytes,
	 * not text; refuse explicitly rather than handing the
	 * model a confusing blob (see CODE-REVIEW.md finding #2).
	 */
	d = dirfstat(fd);
	if(d != nil){
		if(d->qid.type & QTDIR){
			free(d);
			close(fd);
			return esmprint("error: %s is a directory; use list_directory", path);
		}
		free(d);
	}

	data = readfilelimit(fd, Toolreadmax, &truncated);
	close(fd);
	if(data == nil)
		return esmprint("error: read %s: %r", path);
	if(truncated){
		out = esmprint("warning: %s is larger than %d bytes; "
			"output truncated\n%s", path, Toolreadmax, data);
		free(data);
		return out;
	}
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

/*
 * Run a command with stdout and stderr captured through a
 * pipe, optionally chdir'd to dir first.  Returns the combined
 * output (caller frees) or nil with errstr set.
 *
 * Uses wait() and matches on the returned pid instead of
 * waitpid(), which reaps an arbitrary child.  With the srv
 * loop released around prompt rounds, two sessions can run a
 * tool (mk, man) concurrently; waitpid() would let one round
 * reap the other's child, wedging the second runcmd.  Looping
 * on wait() until our own pid comes back keeps each round
 * reaping only its own child.
 */
static char*
runcmd(char *dir, char *cmd, char **argv)
{
	int pfd[2], cpid;
	char *data;
	Waitmsg *w;

	if(pipe(pfd) < 0)
		return nil;
	cpid = fork();
	switch(cpid){
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		return nil;
	case 0:
		close(pfd[0]);
		dup(pfd[1], 1);
		dup(pfd[1], 2);
		close(pfd[1]);
		if(dir != nil && dir[0] != '\0' && chdir(dir) < 0){
			fprint(2, "chdir %s: %r\n", dir);
			exits("chdir");
		}
		exec(cmd, argv);
		fprint(2, "exec %s: %r\n", cmd);
		exits("exec");
	}
	close(pfd[1]);
	data = readfile(pfd[0]);
	close(pfd[0]);
	while((w = wait()) != nil){
		if(w->pid == cpid){
			free(w);
			break;
		}
		free(w);
	}
	return data;
}

static char*
toolman(char *query)
{
	char *argv[4], *data, *q, *page, qbuf[256];
	int argc;

	if(query == nil || query[0] == '\0')
		return esmprint("error: empty man page query");
	/*
	 * Refuse rather than silently truncate: a truncated query
	 * would look up the wrong page and confuse the model.
	 */
	if(strlen(query) >= sizeof qbuf)
		return esmprint("error: man page query too long (limit %d bytes)",
			(int)sizeof qbuf - 1);

	snprint(qbuf, sizeof qbuf, "%s", query);
	q = qbuf;
	while(*q == ' ')
		q++;

	argc = 0;
	argv[argc++] = "man";
	if(q[0] >= '1' && q[0] <= '9' && (q[1] == ' ' || q[1] == '\t')){
		argv[argc++] = q;	/* section */
		q[1] = '\0';
		page = q + 2;
		while(*page == ' ' || *page == '\t')
			page++;
		if(*page == '\0')
			return esmprint("error: missing page name after section %s", q);
		argv[argc++] = page;
	} else
		argv[argc++] = q;
	argv[argc] = nil;

	data = runcmd(nil, "/bin/man", argv);
	if(data == nil)
		return esmprint("error: man: %r");
	if(data[0] == '\0'){
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
	char *argv[Mkmaxargs], *buf, *p, *data, *out;
	int argc, n;

	if(dir == nil || dir[0] == '\0')
		dir = ".";
	buf = estrdup(args ? args : "");

	argc = 0;
	argv[argc++] = "mk";
	for(p = buf; argc < Mkmaxargs - 1; ){
		while(*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if(*p == '\0')
			break;
		argv[argc++] = p;
		while(*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		if(*p != '\0')
			*p++ = '\0';
	}
	argv[argc] = nil;

	data = runcmd(dir, "/bin/mk", argv);
	free(buf);
	if(data == nil)
		return esmprint("error: mk in %s: %r", dir);

	n = strlen(data);
	if(n > Mkmaxout){
		out = esmprint("mk (in %s): output truncated to %d of %d bytes\n"
			"%.*s\n[... truncated ...]\n",
			dir, Mkmaxout, n, Mkmaxout, data);
		free(data);
		return out;
	}
	if(n == 0){
		free(data);
		return esmprint("mk (in %s): ok (no output)", dir);
	}
	return data;
}

/*
 * Install len bytes of data as the complete contents of path,
 * without ever leaving path holding a partial write if
 * something fails partway through (CODE-REVIEW.md finding #1:
 * the previous implementation truncated path with
 * create(path, OWRITE, ...) and then wrote the replacement in
 * place, so a write failure after the truncate lost the
 * original and could leave path half old, half new).
 *
 * The data is first written in full to a temporary sibling
 * file in the same directory; only once that succeeds is the
 * original removed and the temp file renamed into its place.
 * Plan 9's wstat refuses to rename onto an existing name (see
 * stat(5): "it is an error to change the name to that of an
 * existing file"), so a plain rename-over-the-original isn't
 * available; remove-then-rename is the closest thing to an
 * atomic replace Plan 9 offers.  It leaves a brief window
 * where the name doesn't exist, but it never leaves the name
 * holding a half-written mixture of old and new content, which
 * is the actual failure this function exists to prevent.
 *
 * If the rename step fails after the original is already
 * removed, a best-effort attempt is made to recreate path
 * directly from the same verified bytes, so the caller doesn't
 * end up with no file at all; if even that fails, the verified
 * temp file is deliberately left behind (named in the error)
 * instead of removed, so the fully-written replacement is never
 * silently lost -- it can be recovered by hand.
 *
 * Returns nil on success, or a malloc'd error string (caller
 * frees) on failure.
 */
static char*
installfile(char *path, char *data, long len)
{
	char *tmp, *leaf, *slash;
	int fd, i;
	Dir d, *orig;

	orig = dirstat(path);
	if(orig == nil)
		return esmprint("error: stat %s before install: %r", path);

	/*
	 * OEXCL prevents an unrelated pre-existing sibling from
	 * being truncated.  Include a counter because several
	 * independent sessions may share a process id and attempt
	 * replacement concurrently.
	 */
	tmp = nil;
	fd = -1;
	for(i = 0; i < 100; i++){
		free(tmp);
		tmp = esmprint("%s.tmp.%d.%d", path, getpid(), i);
		fd = create(tmp, OWRITE|OEXCL, orig->mode & 0777);
		if(fd >= 0)
			break;
	}
	if(fd < 0){
		free(orig);
		free(tmp);
		return esmprint("error: create unique temp file for %s: %r", path);
	}
	if(writeall(fd, data, len) < 0){
		close(fd);
		remove(tmp);
		free(orig);
		free(tmp);
		return esmprint("error: write temp file for %s: %r", path);
	}
	close(fd);

	/* Preserve writable metadata that the server permits us to set. */
	nulldir(&d);
	d.mode = orig->mode;
	d.gid = orig->gid;
	if(dirwstat(tmp, &d) < 0){
		remove(tmp);
		free(orig);
		free(tmp);
		return esmprint("error: preserve metadata for %s: %r", path);
	}

	if(remove(path) < 0){
		/* original untouched; the verified copy is disposable */
		remove(tmp);
		free(orig);
		free(tmp);
		return esmprint("error: remove %s before install: %r", path);
	}

	slash = strrchr(path, '/');
	leaf = slash != nil ? slash + 1 : path;
	nulldir(&d);
	d.name = leaf;
	if(dirwstat(tmp, &d) < 0){
		/*
		 * path is already gone; fall back to recreating it
		 * directly from the same verified bytes rather than
		 * leaving the caller with no file at all.
		 */
		fd = create(path, OWRITE, orig->mode & 0777);
		if(fd < 0 || writeall(fd, data, len) < 0){
			if(fd >= 0)
				close(fd);
			free(orig);
			return esmprint("error: rename %s into place failed (%r), "
				"and recreating %s also failed; "
				"verified replacement content is preserved at %s",
				tmp, path, tmp);
		}
		close(fd);
		nulldir(&d);
		d.mode = orig->mode;
		d.gid = orig->gid;
		dirwstat(path, &d);
		remove(tmp);
		free(orig);
		free(tmp);
		return nil;
	}
	free(orig);
	free(tmp);
	return nil;
}

/*
 * replace_string: find old_str in the file, verify it occurs
 * exactly once, replace it with new_str.  Returns a status
 * string (caller frees).
 *
 * This is the content-addressed edit model used by Claude Code's
 * str_replace_editor.  It is safe against stale state: if the
 * file has changed and the old text is no longer present, the
 * operation fails loudly instead of silently corrupting.  It is
 * also safe against a write failure partway through the install
 * (see installfile): the original is never truncated until a
 * complete copy of the replacement is already verified on disk.
 */
static char*
toolreplace(char *path, char *oldstr, char *newstr)
{
	int fd, count;
	long filelen, oldlen, newlen;
	char *data, *p, *match, *result, *err;

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

	err = installfile(path, result, filelen - oldlen + newlen);
	free(result);
	if(err != nil)
		return err;

	if(newlen == 0)
		return esmprint("deleted %ld bytes in %s", oldlen, path);
	return esmprint("replaced %ld bytes with %ld bytes in %s",
		oldlen, newlen, path);
}

/*
 * web_search: scrape a search engine's result page through
 * webfs and return a list of (title, URL) pairs.  There is no
 * search API that works without a key and without cost, so
 * this hits DuckDuckGo's no-JS HTML endpoint
 * (html.duckduckgo.com/html/) and extracts real outbound links
 * from the returned page.
 *
 * Design note on why this scrapes generically instead of
 * targeting DuckDuckGo's specific result markup (CSS classes
 * like "result__a"): this program has no network access of its
 * own to verify what that markup currently looks like, and
 * result-page HTML changes without notice.  Extracting every
 * <a href> anchor's visible text is robust to layout changes at
 * the cost of losing snippets and occasionally picking up a
 * stray navigational link.  The one DuckDuckGo-specific piece
 * that *is* required regardless of layout is unwrapping its
 * click-tracking redirect (every result link is rewritten to
 * "//duckduckgo.com/l/?uddg=<percent-encoded target>&...");
 * without that step every link would point back at
 * duckduckgo.com's redirector instead of the real target, and
 * filtering out duckduckgo.com hosted links (to drop the site's
 * own nav/footer/settings links) would remove the results too.
 * That redirect scheme has been stable for years across many
 * independent scraping tools, so it is a safer bet than the
 * page's visual layout.
 *
 * Only ever fetches this one fixed search URL: there is no
 * fetch_url tool and this function does not take one, so the
 * model cannot use it to make claude9fs issue an HTTP request
 * to an arbitrary attacker-chosen host (see README's safety
 * section).  The query text itself does leave the machine (as
 * the DuckDuckGo query string), which is the real risk to be
 * aware of: prompt-injected content could induce a search for
 * sensitive text, leaking it to the search engine and the
 * network path to reach it.
 */

enum {
	Websearchfetchmax = 131072,	/* cap on the raw results page fetched */
	Maxsearchresults = 8,
	Searchsnippetlen = 2000,	/* fallback raw-text dump size */
};

typedef struct Searchresult Searchresult;
struct Searchresult {
	char *title;
	char *href;
};

static void
freesearchresults(Searchresult *r, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(r[i].title);
		free(r[i].href);
	}
}

/* Minimal RFC 3986 percent-encoding for a query string value. */
static char*
urlencode(char *s)
{
	Fmt f;
	uchar c;

	fmtstrinit(&f);
	for(; *s != '\0'; s++){
		c = *s;
		if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
		|| (c >= '0' && c <= '9')
		|| c == '-' || c == '.' || c == '_' || c == '~')
			fmtprint(&f, "%c", c);
		else
			fmtprint(&f, "%%%02X", c);
	}
	return fmtstrflush(&f);
}

static int
hexval(int c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Percent- (and '+'-) decode the byte range [s, end). */
static char*
urldecoden(char *s, char *end)
{
	Fmt f;
	int hi, lo;

	fmtstrinit(&f);
	for(; s < end; s++){
		if(*s == '%' && s + 2 < end
		&& (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0){
			fmtprint(&f, "%c", (hi << 4) | lo);
			s += 2;
		} else if(*s == '+')
			fmtprint(&f, " ");
		else
			fmtprint(&f, "%c", *s);
	}
	return fmtstrflush(&f);
}

/*
 * Decode the handful of HTML entities that show up in scraped
 * result pages, in place (the result is never longer than the
 * input).  Not a general entity decoder: numeric entities
 * (&#NN;) are left as-is, which is a cosmetic shortcoming, not
 * a correctness one.
 */
static void
htmlunescape(char *s)
{
	char *r, *w;

	r = w = s;
	while(*r != '\0'){
		if(*r == '&'){
			if(strncmp(r, "&amp;", 5) == 0){ *w++ = '&'; r += 5; continue; }
			if(strncmp(r, "&lt;", 4) == 0){ *w++ = '<'; r += 4; continue; }
			if(strncmp(r, "&gt;", 4) == 0){ *w++ = '>'; r += 4; continue; }
			if(strncmp(r, "&quot;", 6) == 0){ *w++ = '"'; r += 6; continue; }
			if(strncmp(r, "&#39;", 5) == 0){ *w++ = '\''; r += 5; continue; }
			if(strncmp(r, "&apos;", 6) == 0){ *w++ = '\''; r += 6; continue; }
			if(strncmp(r, "&nbsp;", 6) == 0){ *w++ = ' '; r += 6; continue; }
		}
		*w++ = *r++;
	}
	*w = '\0';
}

/* Strip "<...>" tag spans from [start, end), returning plain text. */
static char*
striptagsrange(char *start, char *end)
{
	Fmt f;
	char *p;
	int intag;

	fmtstrinit(&f);
	intag = 0;
	for(p = start; p < end; p++){
		if(*p == '<'){ intag = 1; continue; }
		if(*p == '>'){ intag = 0; continue; }
		if(!intag)
			fmtprint(&f, "%c", *p);
	}
	return fmtstrflush(&f);
}

/* Collapse runs of whitespace to one space and trim both ends. */
static char*
collapsews(char *s)
{
	Fmt f;
	int lastspace, wrote;

	fmtstrinit(&f);
	lastspace = 1;	/* suppress leading space */
	wrote = 0;
	for(; *s != '\0'; s++){
		if(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'){
			if(!lastspace && wrote)
				fmtprint(&f, " ");
			lastspace = 1;
		} else {
			fmtprint(&f, "%c", *s);
			lastspace = 0;
			wrote = 1;
		}
	}
	return fmtstrflush(&f);
}

static char*
substrdup(char *start, char *end)
{
	char *s;
	long n;

	n = end - start;
	s = emalloc(n + 1);
	memmove(s, start, n);
	s[n] = '\0';
	return s;
}

/*
 * Find attr="..." within the byte range [tagstart, tagend)
 * (tagstart points at '<', tagend at the matching '>') and
 * return its raw (still HTML-entity-escaped) value, or nil.
 */
static char*
findattr(char *tagstart, char *tagend, char *attr)
{
	char needle[64];
	int alen;
	char *p, *v, *q;

	snprint(needle, sizeof needle, "%s=\"", attr);
	alen = strlen(needle);
	for(p = tagstart; p + alen <= tagend; p++){
		if(memcmp(p, needle, alen) == 0){
			v = p + alen;
			q = memchr(v, '"', tagend - v);
			if(q == nil)
				return nil;
			return substrdup(v, q);
		}
	}
	return nil;
}

/*
 * Resolve a raw href attribute value to an absolute URL,
 * unwrapping DuckDuckGo's "/l/?uddg=<encoded target>" redirect
 * (see the toolwebsearch comment) and fixing up protocol-
 * relative ("//host/path") URLs.  Always returns a malloc'd
 * string; may not be a valid absolute URL if the input wasn't
 * (callers filter on the http(s) prefix afterward).
 */
static char*
resolvehref(char *raw)
{
	char *href, *u, *amp, *real;

	href = estrdup(raw);
	htmlunescape(href);

	u = strstr(href, "uddg=");
	if(u != nil){
		u += 5;
		amp = strchr(u, '&');
		real = urldecoden(u, amp != nil ? amp : u + strlen(u));
		free(href);
		return real;
	}
	if(href[0] == '/' && href[1] == '/'){
		real = esmprint("https:%s", href);
		free(href);
		return real;
	}
	return href;
}

/*
 * Scan every <a ...>text</a> anchor in [html, end) and collect
 * external http(s) links with non-empty text into out (up to
 * max entries).  Links that resolve back to skiphost (the
 * search engine's own domain, e.g. its nav/footer/settings
 * links, or an un-unwrapped self-link) are dropped, as are
 * consecutive duplicate hrefs (some result templates repeat an
 * icon link and a text link to the same target).  Returns the
 * number of results collected.
 */
static int
extractlinks(char *html, char *end, char *skiphost,
	Searchresult *out, int max)
{
	char *p, *tagstart, *tagend, *rawhref, *href, *text, *lasthref;
	int n;

	n = 0;
	lasthref = nil;
	p = html;
	while(n < max && (p = strstr(p, "<a ")) != nil){
		tagstart = p;
		tagend = memchr(tagstart, '>', end - tagstart);
		if(tagend == nil)
			break;
		p = tagend + 1;

		rawhref = findattr(tagstart, tagend, "href");
		if(rawhref == nil)
			continue;
		href = resolvehref(rawhref);
		free(rawhref);

		if(strncmp(href, "http://", 7) != 0
		&& strncmp(href, "https://", 8) != 0){
			free(href);
			continue;
		}
		if(skiphost != nil && strstr(href, skiphost) != nil){
			free(href);
			continue;
		}
		text = striptagsrange(tagend + 1,
			strstr(tagend, "</a>") != nil ? strstr(tagend, "</a>") : end);
		htmlunescape(text);
		{
			char *collapsed = collapsews(text);
			free(text);
			text = collapsed;
		}
		if(text[0] == '\0'){
			free(href);
			free(text);
			continue;
		}

		if(lasthref != nil && strcmp(href, lasthref) == 0){
			/*
			 * Same target as the previous accepted link
			 * (e.g. an icon anchor immediately followed by
			 * the readable text anchor, or vice versa).
			 * Keep whichever anchor text is longer, on the
			 * heuristic that a longer string is more likely
			 * to be the real title than icon/alt text.
			 */
			if(n > 0 && strlen(text) > strlen(out[n-1].title)){
				free(out[n-1].title);
				out[n-1].title = text;
			} else
				free(text);
			free(href);
			continue;
		}

		out[n].href = href;
		out[n].title = text;
		free(lasthref);
		lasthref = estrdup(href);
		n++;
	}
	free(lasthref);
	return n;
}

/* Auth for the search fetch: none, just a browser-like UA. */
static int
websearchheaders(int fd, Conv *c)
{
	USED(c);
	return fprint(fd,
		"headers User-Agent: Mozilla/5.0 (compatible; claude9-websearch/1.0)\r\n") < 0
		? -1 : 0;
}

static Provider websearchprov = {
	"websearch", nil, nil, websearchheaders, nil, nil, nil
};

enum {
	Maxkepturls = 64,	/* cap on remembered web_search result URLs per session */
};

/*
 * Record url as fetchable via web_fetch for the rest of this
 * conversation: it came out of a real web_search result, so a
 * later web_fetch for it is not an arbitrary-host request the
 * model invented on its own (see the toolwebfetch doc comment
 * below).  Deduplicates; evicts the oldest entry (FIFO) once
 * Maxkepturls is reached instead of growing without bound over
 * a very long session.
 */
static void
rememberurl(Conv *c, char *url)
{
	int i;

	if(c == nil)
		return;
	for(i = 0; i < c->nsearchurls; i++)
		if(strcmp(c->searchurls[i], url) == 0)
			return;	/* already remembered */
	if(c->nsearchurls >= Maxkepturls){
		free(c->searchurls[0]);
		memmove(c->searchurls, c->searchurls + 1,
			(Maxkepturls - 1) * sizeof(char*));
		c->nsearchurls--;
	}
	c->searchurls = erealloc(c->searchurls,
		(c->nsearchurls + 1) * sizeof(char*));
	c->searchurls[c->nsearchurls++] = estrdup(url);
}

/* True if url exactly matches a previously remembered web_search result. */
static int
urlsearched(Conv *c, char *url)
{
	int i;

	if(c == nil)
		return 0;
	for(i = 0; i < c->nsearchurls; i++)
		if(strcmp(c->searchurls[i], url) == 0)
			return 1;
	return 0;
}

static char*
toolwebsearch(Conv *c, char *query)
{
	char *enc, *url, *html, *out;
	int fd, clonefd, truncated, n, i;
	Searchresult results[Maxsearchresults];
	Fmt f;

	if(query == nil || query[0] == '\0')
		return esmprint("error: empty search query");

	enc = urlencode(query);
	url = esmprint("https://html.duckduckgo.com/html/?q=%s", enc);
	free(enc);

	fd = webhttp(&websearchprov, c, url, nil, 0, &clonefd);
	free(url);
	if(fd < 0)
		return esmprint("error: web search: %r");

	html = readfilelimit(fd, Websearchfetchmax, &truncated);
	close(fd);
	close(clonefd);
	if(html == nil)
		return esmprint("error: web search: read response: %r");

	n = extractlinks(html, html + strlen(html), "duckduckgo",
		results, Maxsearchresults);
	if(n == 0){
		/*
		 * No links matched -- either a genuinely empty result
		 * set, or (more likely, given this scraper cannot be
		 * tested against the live page from here) a markup
		 * change this extractor doesn't handle.  Fall back to
		 * a raw, tag-stripped, truncated dump so the caller
		 * gets *something* instead of silence.
		 */
		char *stripped, *collapsed;

		stripped = striptagsrange(html, html + strlen(html));
		htmlunescape(stripped);
		collapsed = collapsews(stripped);
		free(stripped);
		free(html);
		if(strlen(collapsed) > Searchsnippetlen)
			collapsed[Searchsnippetlen] = '\0';
		out = esmprint("warning: could not parse search results for '%s'; "
			"raw page text (truncated):\n%s", query, collapsed);
		free(collapsed);
		return out;
	}
	free(html);

	fmtstrinit(&f);
	fmtprint(&f, "web search results for '%s':\n\n", query);
	for(i = 0; i < n; i++){
		rememberurl(c, results[i].href);
		fmtprint(&f, "%d. %s\n   %s\n\n", i + 1, results[i].title, results[i].href);
	}
	freesearchresults(results, n);
	return fmtstrflush(&f);
}

/*
 * web_fetch: retrieve the text content of a URL, but only if
 * that exact URL was returned by a previous web_search call in
 * this same conversation (see rememberurl/urlsearched above).
 *
 * This is the "click a result" pattern some other agentic
 * tools use (e.g. the old ChatGPT browsing plugin's
 * search()/click(id)) rather than a raw fetch_url(url)
 * primitive: the model can only name a URL returned as a real
 * result, rather than any URL it or a prompt-injected document
 * invents.  A searched URL can still be hostile.  In particular,
 * webfs follows redirects before returning control; toolwebfetch
 * rejects redirected content after checking parsed/url, but the
 * redirect target has already been contacted.  This is a
 * content-provenance check, not a complete outbound-host or SSRF
 * boundary (see README's safety-risk list).
 *
 * Like toolwebsearch, this scrapes rather than uses a
 * documented API: strips <script>/<style> bodies (their
 * contents are JS/CSS, not natural-language text, and would
 * otherwise show up as noise after generic tag-stripping),
 * strips all remaining tags, decodes entities, collapses
 * whitespace, and truncates to a bounded size.
 */

enum {
	Webfetchmax = 262144,		/* cap on the raw page fetched */
	Webfetchtextmax = 20000,	/* cap on extracted text returned to the model */
};

/*
 * Remove <tag ...>...</tag> spans (case-insensitive tag name,
 * any attributes) from html; used to drop <script> and <style>
 * bodies before generic tag-stripping.  A tag with no matching
 * close is dropped through to the end of the string, which is
 * a safe (if slightly lossy) default for malformed HTML.
 * Always returns a malloc'd string; frees neither its input
 * nor takes ownership of it.
 */
static char*
stripblock(char *html, char *tag)
{
	Fmt f;
	char openpat[16], closepat[16];
	char *p, *o, *oend, *cl;
	int openlen, closelen, c;

	snprint(openpat, sizeof openpat, "<%s", tag);
	snprint(closepat, sizeof closepat, "</%s>", tag);
	openlen = strlen(openpat);
	closelen = strlen(closepat);

	fmtstrinit(&f);
	p = html;
	for(;;){
		o = cistrstr(p, openpat);
		if(o == nil){
			fmtprint(&f, "%s", p);
			break;
		}
		/*
		 * Require a tag boundary right after the name (one of
		 * ">", whitespace, or "/") so e.g. "<scripted>" is not
		 * mistaken for "<script".
		 */
		c = o[openlen];
		if(c != '>' && c != ' ' && c != '\t' && c != '\n'
		&& c != '\r' && c != '/'){
			fmtprint(&f, "%.*s", (int)(o + openlen - p), p);
			p = o + openlen;
			continue;
		}
		fmtprint(&f, "%.*s", (int)(o - p), p);
		oend = strchr(o, '>');
		if(oend == nil)
			break;	/* malformed: drop the rest */
		cl = cistrstr(oend, closepat);
		if(cl == nil)
			break;	/* no closing tag: drop the rest */
		p = cl + closelen;
	}
	return fmtstrflush(&f);
}

/* Auth for the fetch: none, just a browser-like UA (shared with search). */
static Provider webfetchprov = {
	"webfetch", nil, nil, websearchheaders, nil, nil, nil
};

static char*
webfinalurl(int clonefd)
{
	char buf[64], *path, *s;
	int n, fd;

	if(seek(clonefd, 0, 0) < 0)
		return nil;
	n = read(clonefd, buf, sizeof buf - 1);
	if(n <= 0)
		return nil;
	buf[n] = '\0';
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' '))
		buf[--n] = '\0';
	path = esmprint("/mnt/web/%s/parsed/url", buf);
	fd = open(path, OREAD);
	free(path);
	if(fd < 0)
		return nil;
	s = readfile(fd);
	close(fd);
	return s;
}

static int
fetchurlallowed(char *requested, char *final)
{
	return requested != nil && final != nil
		&& strcmp(requested, final) == 0;
}

static char*
toolwebfetch(Conv *c, char *url)
{
	char *html, *final, *noscript, *nostyle, *stripped, *collapsed, *out;
	int fd, clonefd, truncated;

	if(url == nil || url[0] == '\0')
		return esmprint("error: empty url");
	if(strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
		return esmprint("error: url must be http:// or https://");
	if(!urlsearched(c, url))
		return esmprint("error: '%s' was not returned by a previous "
			"web_search result in this session; call web_search "
			"first and pass a URL exactly as it came back", url);

	fd = webhttp(&webfetchprov, c, url, nil, 0, &clonefd);
	if(fd < 0)
		return esmprint("error: web fetch: %r");

	/*
	 * webfs follows redirects before returning body.  Fail
	 * closed unless its final parsed URL is byte-for-byte the
	 * searched URL: redirected content must not enter the model.
	 * This check occurs after webfs followed the redirect, so it
	 * limits returned content, not the network connection itself;
	 * the README documents that residual SSRF-like exposure.
	 */
	final = webfinalurl(clonefd);
	if(!fetchurlallowed(url, final)){
		close(fd);
		close(clonefd);
		out = final == nil
			? esmprint("error: web fetch: cannot verify final URL")
			: esmprint("error: web fetch: redirect rejected: %s -> %s",
				url, final);
		free(final);
		return out;
	}
	free(final);

	html = readfilelimit(fd, Webfetchmax, &truncated);
	close(fd);
	close(clonefd);
	if(html == nil)
		return esmprint("error: web fetch: read response: %r");

	noscript = stripblock(html, "script");
	free(html);
	nostyle = stripblock(noscript, "style");
	free(noscript);
	stripped = striptagsrange(nostyle, nostyle + strlen(nostyle));
	free(nostyle);
	htmlunescape(stripped);
	collapsed = collapsews(stripped);
	free(stripped);

	if(strlen(collapsed) > Webfetchtextmax){
		collapsed[Webfetchtextmax] = '\0';
		out = esmprint("content of %s (truncated to %d bytes):\n\n%s",
			url, Webfetchtextmax, collapsed);
	} else
		out = esmprint("content of %s:\n\n%s", url, collapsed);
	free(collapsed);
	return out;
}

/*
 * Execute a tool call. Returns result string (caller frees).
 * args[] is in Tooldef param order: args[0] is the path.
 */
static char*
exectool(Conv *c, ToolCall *tc)
{
	char *path;
	int fd;

	/*
	 * Once a prompt is cancelled, no further tool starts.  The
	 * call still gets a result: every tool_use needs a
	 * tool_result or the conversation is left malformed.
	 */
	if(cancelled(c))
		return estrdup("not executed: " Cancelmsg);

	path = tc->args[0];
	switch(tc->type){
	case Acreate:
		mkparents(path);
		fd = create(path, OWRITE, 0666);
		if(fd < 0)
			return esmprint("error: create %s: %r", path);
		if(writeall(fd, tc->args[1], strlen(tc->args[1])) < 0){
			close(fd);
			return esmprint("error: write %s: %r", path);
		}
		close(fd);
		return esmprint("created %s (%d bytes)",
			path, (int)strlen(tc->args[1]));

	case Areplace:
		return toolreplace(path, tc->args[1], tc->args[2]);

	case Adelete:
		if(remove(path) < 0)
			return esmprint("error: remove %s: %r", path);
		return esmprint("deleted %s", path);

	case Aread:
		return toolread(path);

	case Alist:
		return toollist(path);

	case Amanpage:
		return toolman(path);

	case Amk:
		return toolmk(path, tc->args[1]);

	case Awebsearch:
		return toolwebsearch(c, path);

	case Awebfetch:
		return toolwebfetch(c, path);
	}

	return esmprint("error: unknown tool '%s'",
		tc->name ? tc->name : "");
}

/*
 * Number of processors, discovered the way mk(1) does ($NPROC,
 * set by the kernel at boot) with stats(8)'s method as the
 * fallback (one line per cpu in /dev/sysstat).  Clamped to
 * [1,32] and cached; used to size runtools' bucket array.
 */
static int
ncpu(void)
{
	static int n;
	char *s, buf[512];
	int fd, i, m;

	if(n > 0)
		return n;
	s = getenv("NPROC");
	if(s != nil){
		n = atoi(s);
		free(s);
	}
	if(n <= 0){
		fd = open("/dev/sysstat", OREAD);
		if(fd >= 0){
			while((m = read(fd, buf, sizeof buf)) > 0)
				for(i = 0; i < m; i++)
					if(buf[i] == '\n')
						n++;
			close(fd);
		}
	}
	if(n < 1)
		n = 1;
	if(n > 32)
		n = 32;
	return n;
}

/*
 * FNV-1a over the cleanname'd path.  Same file named the same
 * way hashes identically, so same-path tool calls land in the
 * same bucket; trivial spelling variants (/x//y, /x/./y) are
 * normalized away.  Bind aliases and relative-vs-absolute
 * names of one file are not detected -- exactly the status quo
 * before bucketing existed.
 *
 * An empty path (e.g. the mk tool's "current directory") is
 * mapped to "." before the copy: cleanname(2) rewrites "" to
 * "." in place and so requires room for at least two bytes,
 * but estrdup("") allocates only one -- a one-byte heap
 * overflow that corrupts the malloc arena and eventually
 * kills the whole process.  "" and "." also name the same
 * directory, so they belong in the same bucket anyway.
 */
static u32int
pathhash(char *path)
{
	u32int h;
	uchar *p;
	char *clean;

	if(path == nil || path[0] == '\0')
		path = ".";
	clean = estrdup(path);
	cleanname(clean);
	h = 2166136261U;
	for(p = (uchar*)clean; *p != '\0'; p++){
		h ^= *p;
		h *= 16777619;
	}
	free(clean);
	return h;
}

/* one bucket of exectool() calls running in its own proc; see runtools */
typedef struct Toolwork Toolwork;
struct Toolwork {
	Conv *c;
	ToolCall *tc;	/* head of bucket chain, linked by bnext */
	Channel *done;	/* elsize sizeof(ToolCall*); shared by all workers of a round */
};

static void
toolworker(void *v)
{
	Toolwork *w;
	ToolCall *tc;

	w = v;
	for(tc = w->tc; tc != nil; tc = tc->bnext)
		tc->result = exectool(w->c, tc);
	sendp(w->done, w->tc);
	free(w);
}

/*
 * Run every tool call from one assistant turn and fill in each
 * tc->result.  A turn with a single tool_use (by far the common
 * case) calls exectool directly, with no extra procs.
 *
 * A turn with several tool_use blocks -- e.g. the model firing
 * off prompts to multiple sub-agent sessions, or several
 * independent read_file calls -- runs them concurrently, but
 * never lets two calls that name the same path run at once.
 * Each call's path (pathhash of args[0]) selects one of ncpu()
 * buckets; each nonempty bucket becomes one Plan 9 process
 * (proccreate: shares memory, the fd table, and the namespace
 * with the caller, so tc->result is written directly and every
 * tool still sees the same mounts) that executes its chain
 * sequentially, in the order the model issued the calls.  Same
 * path, same hash, same bucket: batched edits to one file apply
 * in issue order instead of racing whole-file read-modify-write
 * cycles, a lost-update race that could silently drop an edit
 * or truncate the file.  Distinct paths sharing a bucket merely
 * serialize, costing only parallelism there are no cpus for
 * anyway; and the bucket count caps a round's fan-out at ncpu()
 * procs where it was previously unbounded.  Without concurrency
 * across buckets, a slow call (a sub-agent's whole prompt
 * round, an mk invocation) would stall every other tool_use in
 * the same turn, even though nothing about them is related.
 *
 * Each ToolCall is touched by exactly one worker, so no locking
 * is needed around tc->result itself.  Completion is reported
 * over an unbuffered channel and this function drains exactly
 * one message per worker before returning, so no worker proc
 * outlives the call and the round's tool_result list is only
 * built once every result is in.
 *
 * exectool's own concurrency hazards (e.g. runcmd's fork+wait
 * for mk/man) are already safe under this: wait(2) only ever
 * collects children of the calling process, and each worker is
 * its own process, so two workers' runcmd children can never
 * cross-reap each other -- the same reasoning that already
 * covers two sessions running mk at once (see runcmd's comment).
 */
static void
runtools(Conv *c, ToolCall *calls, void (*cb)(char*, void*), void *aux)
{
	ToolCall *tc, **bucket, **btail;
	Toolwork *w;
	Channel *done;
	char marker[256];
	int n, nb, nw, i;

	n = 0;
	for(tc = calls; tc != nil; tc = tc->next){
		if(cb != nil){
			snprint(marker, sizeof marker,
				"\n[running %s %s]\n",
				tc->name, tc->args[0]);
			cb(marker, aux);
		}
		n++;
	}
	if(n == 0)
		return;
	if(n == 1){
		calls->result = exectool(c, calls);
		return;
	}

	nb = ncpu();
	if(nb > n)
		nb = n;
	bucket = emallocz(nb * sizeof(ToolCall*), 1);
	btail = emallocz(nb * sizeof(ToolCall*), 1);
	for(tc = calls; tc != nil; tc = tc->next){
		/*
		 * web_search and web_fetch both mutate the shared
		 * Conv.searchurls list (rememberurl/urlsearched) with
		 * no locking of their own, unlike every other tool,
		 * which only ever touches its own ToolCall and the
		 * file named in args[0].  Hash them all to one fixed
		 * key instead of their actual argument (a search query
		 * or a URL) so a turn that batches several such calls
		 * -- e.g. two web_searches, or a search plus a fetch of
		 * one of its own results -- lands them in the same
		 * bucket and runs them one at a time, in issue order,
		 * exactly like same-path file edits.  Without this, two
		 * of these calls can land in different buckets and run
		 * in concurrent procs that share memory, and
		 * rememberurl's realloc of c->searchurls from one proc
		 * can be freed out from under a concurrent reader/
		 * writer in another -- malloc arena corruption, not
		 * just a wrong answer.
		 */
		if(tc->type == Awebsearch || tc->type == Awebfetch)
			i = pathhash("/web") % nb;
		else
			i = pathhash(tc->args[0]) % nb;
		tc->bnext = nil;
		if(btail[i] == nil)
			bucket[i] = tc;
		else
			btail[i]->bnext = tc;
		btail[i] = tc;
	}

	done = chancreate(sizeof(ToolCall*), 0);
	nw = 0;
	for(i = 0; i < nb; i++){
		if(bucket[i] == nil)
			continue;
		w = emalloc(sizeof *w);
		w->c = c;
		w->tc = bucket[i];
		w->done = done;
		proccreate(toolworker, w, 32*1024);
		nw++;
	}
	while(nw-- > 0)
		recvp(done);
	chanfree(done);
	free(bucket);
	free(btail);
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
	Sbuf text;
	Sbuf thinking;	/* thinking blocks: streamed text... */
	Sbuf sig;	/* ...plus closing signature */
	Sbuf tooljson;
	char *redacted;	/* redacted_thinking: opaque data blob */
	char *toolid;
	char *toolname;
	char *opaque;	/* complete server-side content block JSON */
};

/*
 * Append n bytes of s to a growable buffer, keeping it
 * NUL-terminated.
 */
void
sbappend(Sbuf *b, char *s, int n)
{
	int need;

	need = b->len + n + 1;
	if(need > b->cap){
		while(need > b->cap)
			b->cap = b->cap ? b->cap * 2 : 256;
		b->s = erealloc(b->s, b->cap);
	}
	memmove(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
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
		if(blocks[i].opaque != nil){
			block = jsonparse(blocks[i].opaque);
			if(block != nil)
				jappend(content, block);
			continue;
		}
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
					jstring(blocks[i].thinking.s ? blocks[i].thinking.s : ""));
				jset(block, "signature",
					jstring(blocks[i].sig.s ? blocks[i].sig.s : ""));
			}
			jappend(content, block);
			continue;
		}
		if(!blocks[i].istool){
			/*
			 * Skip empty AND whitespace-only text blocks:
			 * the API rejects both ("text content blocks
			 * must contain non-whitespace text").  A text
			 * delta of just "\n" or " " right before a tool
			 * call is common and must not be stored in the
			 * rawjson we replay on later rounds, or every
			 * resend wedges the conversation.
			 */
			if(blankstr(blocks[i].text.s))
				continue;
			block = jobject();
			jset(block, "type", jstring("text"));
			jset(block, "text", jstring(blocks[i].text.s));
			jappend(content, block);
			fmtprint(&f, "%s", blocks[i].text.s);
			continue;
		}

		block = jobject();
		jset(block, "type", jstring("tool_use"));
		jset(block, "id",
			jstring(blocks[i].toolid ? blocks[i].toolid : ""));
		jset(block, "name",
			jstring(blocks[i].toolname ? blocks[i].toolname : ""));
		if(blocks[i].tooljson.len > 0)
			input = jsonparse(blocks[i].tooljson.s);
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
		tc->type = td != nil ? td->type : -1;
		parseinput(tc, td, input);
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
	if(stop_reason != nil && strcmp(stop_reason, "pause_turn") == 0)
		r->paused = 1;
	return r;
}

static void
freeblocks(Sblock *blocks, int nblocks)
{
	int i;
	for(i = 0; i < nblocks; i++){
		free(blocks[i].text.s);
		free(blocks[i].thinking.s);
		free(blocks[i].sig.s);
		free(blocks[i].tooljson.s);
		free(blocks[i].redacted);
		free(blocks[i].toolid);
		free(blocks[i].toolname);
		free(blocks[i].opaque);
	}
}

/*
 * Look up (and extend the count to cover) the content block
 * addressed by the event's index.  An index past Maxblocks
 * fails the round loudly: silently dropping a block would
 * desync the tool protocol (a tool_use with no tool_result
 * wedges the conversation).
 */
static Sblock*
sseblock(Json *ev, Sblock *blocks, int *nblocksp)
{
	int idx;

	idx = jint(ev, "index");
	if(idx < 0 || idx >= Maxblocks){
		werrstr("content block index %d exceeds limit (%d)",
			idx, Maxblocks);
		return nil;
	}
	if(idx >= *nblocksp)
		*nblocksp = idx + 1;
	return &blocks[idx];
}

/*
 * Handle one SSE event.  Returns 1 on message_stop, -1 on
 * error (errstr set), 0 otherwise.
 */
static int
sseevent(Json *ev, Sblock *blocks, int *nblocksp,
	char **stopreasonp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Json *delta, *cblock, *uobj;
	Sblock *b;
	Sbuf *sb;
	char *etype, *dtype, *s;
	int docb;

	etype = jstr(ev, "type");
	if(etype == nil)
		return 0;

	if(strcmp(etype, "message_stop") == 0)
		return 1;

	if(strcmp(etype, "error") == 0){
		s = jstr(jget(ev, "error"), "message");
		werrstr("API error: %s", s ? s : "unknown");
		return -1;
	}

	if(strcmp(etype, "message_start") == 0){
		uobj = jget(jget(ev, "message"), "usage");
		if(usage != nil && uobj != nil){
			usage->input_tokens += jint(uobj, "input_tokens");
			usage->cache_creation_input_tokens += jint(uobj, "cache_creation_input_tokens");
			usage->cache_read_input_tokens += jint(uobj, "cache_read_input_tokens");
		}
		return 0;
	}

	if(strcmp(etype, "message_delta") == 0){
		s = jstr(jget(ev, "delta"), "stop_reason");
		if(s != nil){
			free(*stopreasonp);
			*stopreasonp = estrdup(s);
		}
		uobj = jget(ev, "usage");
		if(usage != nil && uobj != nil){
			usage->output_tokens += jint(uobj, "output_tokens");
			usage->cache_creation_input_tokens += jint(uobj, "cache_creation_input_tokens");
			usage->cache_read_input_tokens += jint(uobj, "cache_read_input_tokens");
		}
		return 0;
	}

	if(strcmp(etype, "content_block_start") == 0){
		b = sseblock(ev, blocks, nblocksp);
		if(b == nil)
			return -1;
		cblock = jget(ev, "content_block");
		dtype = jstr(cblock, "type");
		if(dtype == nil)
			return 0;
		if(strcmp(dtype, "server_tool_use") == 0
		|| strcmp(dtype, "advisor_tool_result") == 0){
			b->opaque = jsonstr(cblock);
		} else if(strcmp(dtype, "tool_use") == 0){
			b->istool = 1;
			s = jstr(cblock, "id");
			b->toolid = estrdup(s ? s : "");
			s = jstr(cblock, "name");
			b->toolname = estrdup(s ? s : "");
		} else if(strcmp(dtype, "thinking") == 0){
			b->isthinking = 1;
			if(cb != nil) cb("[thinking]\n", aux);
		} else if(strcmp(dtype, "redacted_thinking") == 0){
			b->isthinking = 1;
			s = jstr(cblock, "data");
			b->redacted = estrdup(s ? s : "");
			if(cb != nil) cb("[redacted thinking]\n", aux);
		}
		return 0;
	}

	if(strcmp(etype, "content_block_stop") == 0){
		b = sseblock(ev, blocks, nblocksp);
		if(b == nil)
			return -1;
		if(b->isthinking && b->redacted == nil && cb != nil)
			cb("\n[/thinking]\n", aux);
		return 0;
	}

	if(strcmp(etype, "content_block_delta") == 0){
		b = sseblock(ev, blocks, nblocksp);
		if(b == nil)
			return -1;
		delta = jget(ev, "delta");
		dtype = jstr(delta, "type");
		if(dtype == nil)
			return 0;
		/* route the delta text to the right per-block buffer */
		s = nil;
		sb = nil;
		docb = 0;
		if(strcmp(dtype, "text_delta") == 0){
			s = jstr(delta, "text");
			sb = &b->text;
			docb = 1;
		} else if(strcmp(dtype, "thinking_delta") == 0){
			s = jstr(delta, "thinking");
			sb = &b->thinking;
			docb = 1;
		} else if(strcmp(dtype, "input_json_delta") == 0){
			s = jstr(delta, "partial_json");
			sb = &b->tooljson;
		} else if(strcmp(dtype, "signature_delta") == 0){
			s = jstr(delta, "signature");
			sb = &b->sig;
		}
		if(sb != nil && s != nil){
			sbappend(sb, s, strlen(s));
			if(docb && cb != nil)
				cb(s, aux);
		}
		return 0;
	}

	return 0;
}

static int
ssehandle(char *json, Sblock *blocks, int *nblocksp,
	char **stopreasonp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Json *ev;
	int rc;

	ev = jsonparse(json);
	if(ev == nil)
		return 0;
	rc = sseevent(ev, blocks, nblocksp, stopreasonp, usage, cb, aux);
	jsonfree(ev);
	return rc;
}

/*
 * Anthropic: consume a streamed SSE response and reassemble
 * the assistant turn into a Reply.
 */
static Reply*
anthropicreadstream(Conv *c, Biobuf *bp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	char *stopreason, *line, *p;
	int rc, done, err;
	Sblock blocks[Maxblocks];
	int nblocks;
	Reply *r;

	memset(blocks, 0, sizeof blocks);
	nblocks = 0;
	stopreason = nil;
	done = 0;
	err = 0;

	while(!done && !cancelled(c) && (line = Brdstr(bp, '\n', 1)) != nil){
		if(strncmp(line, "data:", 5) != 0){
			free(line);
			continue;
		}
		p = line + 5;
		while(*p == ' ') p++;
		if(*p == '\0'){ free(line); continue; }
		rc = ssehandle(p, blocks, &nblocks, &stopreason,
			usage, cb, aux);
		free(line);
		if(rc < 0){ err = 1; break; }
		if(rc > 0) done = 1;
	}

	/*
	 * The SSE stream must end with a message_stop event.  If it
	 * just stops (connection drop, webfs hiccup), the response
	 * is incomplete: treat it as an error rather than passing
	 * off partial blocks as a finished turn.  A cancel also ends
	 * the loop early (or breaks the blocked read with
	 * "interrupted"), and must be reported as one, not as a lost
	 * connection.
	 */
	if(!done && !err){
		if(cancelled(c))
			werrstr(Cancelmsg);
		else
			werrstr("response stream ended unexpectedly (connection lost?)");
		err = 1;
	}

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

/*
 * One request/response round through the conversation's
 * provider: build the request, POST it, hand the streamed
 * body to the provider's reader.
 */
static Reply*
sendonce1(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Json *req;
	char *body;
	Biobuf *bp;
	int fd, clonefd;
	Reply *r;
	Provider *p;

	p = provof(c);
	req = p->buildreq(c);
	if(req == nil)
		return nil;	/* errstr set by buildreq */
	jset(req, "stream", jbool(1));
	body = jsonstr(req);
	jsonfree(req);
	if(body == nil){
		werrstr("failed to serialize request");
		return nil;
	}

	fd = webhttp(p, c,
		c->baseurl != nil && c->baseurl[0] != '\0' ? c->baseurl : p->apiurl,
		body, 1, &clonefd);
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

	r = p->readstream(c, bp, usage, cb, aux);
	Bterm(bp);
	close(fd);
	close(clonefd);
	return r;
}

enum {
	Maxquirks = 3,	/* quirk-driven retries per round; see sendonce */
};

/*
 * sendonce1 plus quirk-driven retries: if the round fails and
 * the provider's quirk hook recognizes the error as a fixable
 * request-shape complaint (e.g. an openai-compatible server
 * wanting the legacy max_tokens field name instead of
 * max_completion_tokens), it adjusts the Conv's quirk flags
 * and the request is rebuilt and resent.  The adjustments
 * stick on the Conv, so later rounds get the right shape on
 * their first try.
 *
 * Up to Maxquirks retries per round, because one complaint can
 * take several adjustments to pin down: the reasoning_effort
 * quirk ladder (see openaiquirk) may need to try omitting the
 * field and then sending an explicit "none" before the server
 * is satisfied, and forcing each rung onto a separate user
 * prompt would surface errors the next request was already
 * going to fix.  The cap keeps a misbehaving hook (or a server
 * whose errors toggle a flag back and forth) from retrying
 * forever; well-behaved quirk state machines are monotonic and
 * stop asking on their own.
 *
 * The provider is re-resolved on every attempt because a quirk
 * hook may change it: openaiquirk moves a Conv to the responses
 * provider when a model refuses function tools on Chat
 * Completions (switchresponses), and the retry must then use
 * the new provider's builder, reader, endpoint, and quirk hook.
 */
static Reply*
sendonce(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	Reply *r;
	Provider *p;
	char errbuf[ERRMAX];
	int try;

	for(try = 0;; try++){
		if(cancelled(c)){
			werrstr(Cancelmsg);
			return nil;
		}
		p = provof(c);
		r = sendonce1(c, usage, cb, aux);
		if(r != nil)
			return r;
		/*
		 * A cancelled round fails with whatever the interrupted
		 * call left in errstr ("interrupted"); say what really
		 * happened, and never let a quirk hook mistake it for a
		 * request-shape complaint.
		 */
		if(cancelled(c)){
			werrstr(Cancelmsg);
			return nil;
		}
		if(p->quirk == nil || try >= Maxquirks)
			return nil;
		rerrstr(errbuf, sizeof errbuf);
		if(!p->quirk(c, errbuf)){
			werrstr("%s", errbuf);	/* quirk may have clobbered errstr */
			return nil;
		}
		if(provof(c) != p)
			fprint(2, "claude: %s: switching to %s after request-shape error: %s\n",
				p->name, provof(c)->name, errbuf);
		else
			fprint(2, "claude: %s: retrying after request-shape error: %s\n",
				p->name, errbuf);
	}
}

/*
 * Recognize the API's "context window exceeded" error.  The
 * Anthropic API reports it as an invalid_request_error whose
 * message contains "prompt is too long" (e.g. "prompt is too
 * long: 210000 tokens > 200000 maximum").  We match on the
 * stable substring rather than the numbers, which vary by
 * model.  The string reaches us through weberror() (HTTP 400
 * body) or an SSE error event, both of which prefix it with
 * "API error: ".
 */
int
overlimiterr(char *err)
{
	static char *pat[] = {
		/* Anthropic: "prompt is too long: N tokens > M maximum" */
		"prompt is too long",
		/* OpenAI chat completions and vLLM:
		 * "This model's maximum context length is N tokens ..." */
		"maximum context length",
		/* OpenAI error code, present in some bodies without the prose */
		"context_length_exceeded",
		/* OpenAI Responses: "Your input exceeds the context window
		 * of this model." */
		"exceeds the context window",
		/* older wording; kept for servers that still say it */
		"exceed the context",
		/* llama.cpp server (HTTP 400): "the request exceeds the
		 * available context size, try increasing it", error type
		 * "exceed_context_size_error" */
		"exceeds the available context size",
		"exceed_context_size",
	};
	int i;

	if(err == nil)
		return 0;
	for(i = 0; i < nelem(pat); i++)
		if(strstr(err, pat[i]) != nil)
			return 1;
	return 0;
}

/*
 * The text stored in a session's error file (and returned to a
 * failed prompt write) for a context-overflow error: the raw
 * error followed by the way out.  Kept here, not inline in
 * doprompt, so the advice is testable without a 9P server; see
 * tests.c.  Caller frees.
 */
char*
overlimitmsg(char *err)
{
	return esmprint("%s\n"
		"context window exceeded; this session is wedged "
		"until history shrinks.  Drop old exchanges with "
		"'echo compact > ctl' (optionally 'compact N' to "
		"keep N recent exchanges) and resend, or 'echo clear "
		"> ctl' to start fresh.", err != nil ? err : "");
}

/*
 * True if err is the specific, recoverable "tool loop limit
 * reached" condition raised when claudeconverse exhausts its
 * round cap while the model is still calling tools.  Unlike a
 * real API failure, the conversation is left well-formed
 * (every tool_use answered by a tool_result, ending on a user
 * turn), so it is safe to resume with another prompt.
 *
 * The wording must match the esmprint at the end of
 * claudeconverse exactly.  In particular it must NOT match the
 * "tool/advisor loop limit reached" variant emitted when the
 * cap fell on an advisor pause_turn round: that conversation
 * ends on a replayed assistant turn, and appending a user
 * "Continue." there violates the pause protocol.  (An earlier
 * revision emitted the advisor wording unconditionally, which
 * made this function never match and silently disabled
 * auto-continue for every capped tool loop.)
 */
int
toollimiterr(char *err)
{
	if(err == nil)
		return 0;
	return strstr(err, "tool loop limit reached") != nil;
}

int
cancelled(Conv *c)
{
	return c != nil && c->cancel;
}

int
cancelerr(char *err)
{
	if(err == nil)
		return 0;
	return strstr(err, Cancelmsg) != nil;
}

char*
claudeconverse(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux, char **errp)
{
	Reply *r;
	ToolCall *tc;
	char *resultjson, *alltext, marker[256], errbuf[ERRMAX];
	int round, ntext, maxrounds, lastpaused;
	Fmt f;

	if(errp != nil)
		*errp = nil;
	fmtstrinit(&f);
	ntext = 0;
	/*
	 * Per-conversation round cap (ctl "maxrounds N" in
	 * claude9fs), read once so a mid-loop change cannot make
	 * the loop's bound move under it.
	 */
	maxrounds = c->maxrounds > 0 ? c->maxrounds : Defmaxrounds;
	lastpaused = 0;

	for(round = 0; round < maxrounds; round++){
		/*
		 * Cancelled between rounds (typically while tools were
		 * running).  The conversation ends on the tool_results
		 * just appended, so it is well-formed and resumable, the
		 * same state the round-cap exit leaves.
		 */
		if(c->cancel){
			if(errp != nil)
				*errp = estrdup(Cancelmsg);
			if(cb != nil)
				cb("\n[cancelled]\n", aux);
			return fmtstrflush(&f);
		}
		r = sendonce(c, usage, cb, aux);
		if(r == nil){
			/*
			 * Surface the failure even when we have
			 * partial text from earlier rounds: the
			 * stream gets a marker and the caller gets
			 * the error string via errp.  The failed round
			 * appends nothing, so history stays well-formed
			 * (it ends on the user turn that started the
			 * round); a cancelled round's partial text was
			 * streamed but is not kept.
			 */
			rerrstr(errbuf, sizeof errbuf);
			if(errp != nil)
				*errp = estrdup(errbuf);
			if(cb != nil){
				if(cancelerr(errbuf))
					snprint(marker, sizeof marker, "\n[cancelled]\n");
				else
					snprint(marker, sizeof marker,
						"\n[error: %s]\n", errbuf);
				cb(marker, aux);
			}
			alltext = fmtstrflush(&f);
			if(ntext > 0) return alltext;
			free(alltext);
			return nil;
		}

		if(r->text != nil && r->text[0] != '\0')
			fmtprint(&f, "%s%s", ntext++ ? "\n" : "", r->text);

		convappend(c, msgnew(Massistant,
			r->text ? r->text : "", r->rawjson));

		if(r->paused){
			/*
			 * Anthropic may pause while its server-side advisor is
			 * pending.  Replay the assistant content unchanged and
			 * immediately resend: no user message or tool_result is
			 * permitted on this path.
			 */
			lastpaused = 1;
			replyfree(r);
			continue;
		}
		lastpaused = 0;
		if(r->stopped){
			/*
			 * The max_tokens guillotine can fall mid
			 * tool call, leaving tool_use blocks that
			 * were never executed.  The API demands a
			 * tool_result for every tool_use, so answer
			 * them with a refusal; otherwise the next
			 * message on this conversation (including
			 * autocontinue's "Continue.") is rejected.
			 */
			if(r->tools != nil){
				for(tc = r->tools; tc != nil; tc = tc->next)
					tc->result = estrdup(
						"not executed: response truncated (max_tokens)");
				resultjson = mktoolresults(r->tools);
				convappend(c, msgnew(Muser, "", resultjson));
				free(resultjson);
			}
			replyfree(r);
			return fmtstrflush(&f);
		}
		if(r->tools == nil){
			replyfree(r);
			return fmtstrflush(&f);
		}

		runtools(c, r->tools, cb, aux);

		resultjson = mktoolresults(r->tools);
		convappend(c, msgnew(Muser, "", resultjson));
		free(resultjson);

		replyfree(r);
	}

	/*
	 * Round cap reached.  Two distinct situations, and the
	 * wording tells them apart (toollimiterr keys on it):
	 *
	 * Ordinary case: the model was still asking for tools when
	 * we cut it off.  The conversation ends with tool results
	 * appended (a user turn), so it can be resumed with another
	 * prompt -- claude9fs's auto-continue does exactly that --
	 * but the user must be told this answer is not done.
	 *
	 * Advisor case: the last round was a pause_turn, so the
	 * conversation ends on a replayed assistant turn with no
	 * tool results.  Appending a user message there violates
	 * the pause protocol, so this is reported under a wording
	 * toollimiterr does not match and auto-continue leaves it
	 * alone.
	 */
	if(lastpaused){
		if(errp != nil)
			*errp = esmprint("tool/advisor loop limit reached (%d rounds)", maxrounds);
		if(cb != nil)
			cb("\n[tool/advisor loop limit reached]\n", aux);
	}else{
		if(errp != nil)
			*errp = esmprint("tool loop limit reached (%d rounds)", maxrounds);
		if(cb != nil)
			cb("\n[tool loop limit reached]\n", aux);
	}
	return fmtstrflush(&f);
}

/*
 * Fetch the available model ids, one per line (caller frees).
 * Returns nil with errstr set on failure.
 */
char*
fetchmodels(int prov, char *apikey)
{
	int fd, clonefd, i;
	char *data, *id, *msg;
	Json *resp, *darr;
	Fmt f;
	Provider *p;

	if(prov < 0 || prov >= nelem(providers)){
		werrstr("unknown provider %d", prov);
		return nil;
	}
	p = &providers[prov];
	if(p->modelsurl == nil){
		werrstr("provider %s has no models endpoint", p->name);
		return nil;
	}
	{
		Conv c;
		memset(&c, 0, sizeof c);
		c.apikey = apikey;
		fd = webhttp(p, &c, p->modelsurl, nil, 0, &clonefd);
	}
	if(fd < 0)
		return nil;
	data = readfile(fd);
	close(fd);
	close(clonefd);
	if(data == nil)
		return nil;

	resp = jsonparse(data);
	if(resp == nil){
		werrstr("json parse failed: %.100s", data);
		free(data);
		return nil;
	}
	free(data);

	msg = jstr(jget(resp, "error"), "message");
	if(msg != nil){
		werrstr("models API: %s", msg);
		jsonfree(resp);
		return nil;
	}
	darr = jget(resp, "data");
	if(darr == nil || darr->type != Jarray){
		werrstr("no data array in models response");
		jsonfree(resp);
		return nil;
	}

	fmtstrinit(&f);
	for(i = 0; i < darr->nitem; i++){
		id = jstr(jidx(darr, i), "id");
		if(id != nil)
			fmtprint(&f, "(/model %s)\n", id);
	}
	jsonfree(resp);
	return fmtstrflush(&f);
}
