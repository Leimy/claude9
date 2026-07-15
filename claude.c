#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
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

enum {
	Acreate,	/* tool types */
	Areplace,
	Aread,
	Alist,
	Adelete,
	Amanpage,
	Amk,

	Maxargs = 3,	/* max parameters per tool */
};

/* tool call from Claude */
typedef struct ToolCall ToolCall;
struct ToolCall {
	char *id;		/* tool_use_id */
	char *name;		/* tool name for display */
	int type;		/* Acreate, ...; -1 = unknown tool */
	char *args[Maxargs];	/* values in Tooldef param order; args[0] is the path */
	char *result;		/* result text after execution */
	ToolCall *next;
	ToolCall *bnext;	/* next call in the same hash bucket; see runtools */
};

/*
 * Reply assembled from a single API round: text, tool_use
 * blocks, and a raw JSON snapshot of the assistant's content
 * array suitable for appending back into a follow-up request.
 */
typedef struct Reply Reply;
struct Reply {
	char *text;		/* concatenated text blocks */
	char *rawjson;		/* raw JSON content array string */
	ToolCall *tools;	/* linked list of tool calls */
	int stopped;		/* 1 unless stop_reason was tool_use */
};

/*
 * Single source of truth for the tools we expose to Claude.
 *
 * Each tool takes up to Maxargs string parameters, all
 * required; params[0] is always the path.  The same table
 * drives the JSON schema (mktools) and argument extraction
 * (parseinput); ToolCall.args holds the values in params
 * order.  findtool() maps API names to the Acreate/... enum.
 */
typedef struct Toolparam Toolparam;
struct Toolparam {
	char *name;
	char *desc;
};

typedef struct Tooldef Tooldef;
struct Tooldef {
	int type;
	char *name;
	char *desc;
	Toolparam params[Maxargs];	/* nil name terminates */
};

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
 * Pull parameters out of a tool_use block's "input" object,
 * in Tooldef param order.  Missing parameters (and all
 * parameters of an unknown tool, td == nil) become "".
 */
static void
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
}

void
convfree(Conv *c)
{
	if(c == nil)
		return;
	convclear(c);
	free(c->apikey);
	free(c->model);
	free(c->effort);
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
	m->text = estrdup(text);
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
 * Build tool definitions JSON.
 */
static Json*
mktools(void)
{
	Json *arr, *t, *input, *props, *p, *req, *cc;
	Tooldef *td;
	Toolparam *tp;
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
 * True if s is nil, empty, or contains only whitespace.
 * The API rejects text content blocks that are empty OR
 * whitespace-only ("text content blocks must contain
 * non-whitespace text"), so both cases must be treated
 * identically everywhere a text block is emitted.
 */
static int
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

static Json*
buildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *block, *sys, *cc;
	Msg *m;

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

	jset(req, "tools", mktools());

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
	char *path, *ebody, *emsg;
	Json *ej;
	int fd;

	path = esmprint("%s/errorbody", webdir);
	fd = open(path, OREAD);
	free(path);
	if(fd < 0)
		return;	/* keep errstr from the failed body open */
	ebody = readfile(fd);
	close(fd);
	if(ebody == nil)
		return;
	ej = jsonparse(ebody);
	emsg = jstr(jget(ej, "error"), "message");
	if(emsg != nil)
		werrstr("API error: %s", emsg);
	else
		werrstr("API error: %.256s", ebody);
	jsonfree(ej);
	free(ebody);
}

/*
 * Perform an HTTP request to the Anthropic API through webfs.
 * postbody nil means GET.  On success returns an open fd for
 * the response body and stores the connection fd in *clonefdp
 * (caller closes both); on error returns -1 with errstr set.
 */
static int
webhttp(char *apikey, char *url, char *postbody, int stream, int *clonefdp)
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
	|| fprint(fd, "headers x-api-key: %s\r\n", apikey) < 0
	|| fprint(fd, "headers anthropic-version: %s\r\n", apiversion) < 0){
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

static void
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

static void
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
 * args[] is in Tooldef param order: args[0] is the path.
 */
static char*
exectool(ToolCall *tc)
{
	char *path;
	int fd;

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
		tc->result = exectool(tc);
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
runtools(ToolCall *calls, void (*cb)(char*, void*), void *aux)
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
		calls->result = exectool(calls);
		return;
	}

	nb = ncpu();
	if(nb > n)
		nb = n;
	bucket = emallocz(nb * sizeof(ToolCall*), 1);
	btail = emallocz(nb * sizeof(ToolCall*), 1);
	for(tc = calls; tc != nil; tc = tc->next){
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
		if(strcmp(dtype, "tool_use") == 0){
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

	fd = webhttp(c->apikey, apiurl, body, 1, &clonefd);
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

	/*
	 * The SSE stream must end with a message_stop event.  If it
	 * just stops (connection drop, webfs hiccup), the response
	 * is incomplete: treat it as an error rather than passing
	 * off partial blocks as a finished turn.
	 */
	if(!done && !err){
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
	if(err == nil)
		return 0;
	return strstr(err, "prompt is too long") != nil
		|| strstr(err, "exceed the context") != nil
		|| strstr(err, "maximum context length") != nil;
}

enum {
	Maxrounds = 20,	/* tool-loop round cap per prompt */
};

/*
 * True if err is the specific, recoverable "tool loop limit
 * reached" condition raised when claudeconverse exhausts
 * Maxrounds while the model is still calling tools.  Unlike a
 * real API failure, the conversation is left well-formed
 * (every tool_use answered by a tool_result, ending on a user
 * turn), so it is safe to resume with another prompt.  The
 * exact wording must match the esmprint below.
 */
int
toollimiterr(char *err)
{
	if(err == nil)
		return 0;
	return strstr(err, "tool loop limit reached") != nil;
}

char*
claudeconverse(Conv *c, Usage *usage,
	void (*cb)(char*, void*), void *aux, char **errp)
{
	Reply *r;
	ToolCall *tc;
	char *resultjson, *alltext, marker[256], errbuf[ERRMAX];
	int round, ntext;
	Fmt f;

	if(errp != nil)
		*errp = nil;
	fmtstrinit(&f);
	ntext = 0;

	for(round = 0; round < Maxrounds; round++){
		r = sendonce(c, usage, cb, aux);
		if(r == nil){
			/*
			 * Surface the failure even when we have
			 * partial text from earlier rounds: the
			 * stream gets a marker and the caller gets
			 * the error string via errp.
			 */
			rerrstr(errbuf, sizeof errbuf);
			if(errp != nil)
				*errp = estrdup(errbuf);
			if(cb != nil){
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

		runtools(r->tools, cb, aux);

		resultjson = mktoolresults(r->tools);
		convappend(c, msgnew(Muser, "", resultjson));
		free(resultjson);

		replyfree(r);
	}

	/*
	 * Tool-loop round cap reached: the model was still asking
	 * for tools when we cut it off.  The conversation ends with
	 * tool results appended, so it can be resumed with another
	 * prompt, but the user must be told this answer is not done.
	 */
	if(errp != nil)
		*errp = esmprint("tool loop limit reached (%d rounds)", Maxrounds);
	if(cb != nil)
		cb("\n[tool loop limit reached; send another prompt to continue]\n", aux);
	return fmtstrflush(&f);
}

/*
 * Fetch the available model ids, one per line (caller frees).
 * Returns nil with errstr set on failure.
 */
char*
fetchmodels(char *apikey)
{
	int fd, clonefd, i;
	char *data, *id, *msg;
	Json *resp, *darr;
	Fmt f;

	fd = webhttp(apikey, modelsurl, nil, 0, &clonefd);
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
