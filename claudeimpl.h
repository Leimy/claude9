/*
 * Internal interface between claude.c (conversation engine,
 * tool execution, transport, anthropic provider) and the
 * other per-provider wire-format implementations (openai.c).
 * Clients of the library (claude9fs.c, claudetalk) must use
 * claude.h only; nothing here is part of the public API.
 *
 * Plan 9 style: includers must include u.h, libc.h, bio.h,
 * json.h, and claude.h first.
 *
 * NOTE (kencc -T type signatures): every struct here must be
 * fully defined in this header so all translation units hash
 * identical signatures for symbols whose types touch them.
 * No extern data is declared here for the same reason (sized
 * vs unsized array declarations hash differently); use the
 * tooldef() accessor instead of reaching for the tools table.
 */

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

/* one tool invocation requested by the model */
typedef struct ToolCall ToolCall;
struct ToolCall {
	char *id;		/* provider's tool call id */
	char *name;		/* tool name for display */
	int type;		/* Acreate, ...; -1 = unknown tool */
	char *args[Maxargs];	/* values in Tooldef param order; args[0] is the path */
	char *result;		/* result text after execution */
	ToolCall *next;
	ToolCall *bnext;	/* next call in the same hash bucket; see runtools */
};

/*
 * Tool definitions: the single source of truth for the tools
 * exposed to the model lives in claude.c (Tooldef tools[]);
 * providers reach it through tooldef(i).  Each tool takes up
 * to Maxargs string parameters, all required; params[0] is
 * always the path.
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

/*
 * Reply assembled from a single API round: text, tool calls,
 * and a raw JSON snapshot of the assistant's content in the
 * NEUTRAL content-array form (see PROVIDERS.md): an array of
 * {text | thinking | redacted_thinking | tool_use} blocks in
 * the anthropic spelling.  Every provider's readstream must
 * emit rawjson in this form; every provider's buildreq must
 * accept it when replaying history.
 */
typedef struct Reply Reply;
struct Reply {
	char *text;		/* concatenated text blocks */
	char *rawjson;		/* neutral JSON content array */
	ToolCall *tools;	/* linked list of tool calls */
	int stopped;		/* 1 unless the model stopped to call tools */
};

/*
 * Provider vtable: everything wire-format-specific lives
 * behind these entries, so the tool loop, conversation
 * storage, and transport stay provider-neutral.  See
 * PROVIDERS.md for the design notes.
 *
 * headers	write the provider's auth header lines to the
 *		webfs ctl fd; returns <0 on write error.
 * buildreq	assemble the full request JSON from the
 *		conversation, except the "stream" flag, which
 *		sendonce adds (both known providers spell it
 *		the same way).  Returns nil with errstr set on
 *		failure.
 * readstream	consume the streamed response body from bp and
 *		reassemble the assistant turn as a Reply (with
 *		neutral rawjson; see above).  The caller owns
 *		bp and the underlying fds.  Returns nil with
 *		errstr set on failure.  Must normalize the
 *		stop reason recorded in Usage to the anthropic
 *		names: tool_use, end_turn, max_tokens.
 * quirk	optional (may be nil).  Called by sendonce when
 *		a round fails, with the error string.  If the
 *		error is a fixable request-shape complaint
 *		(e.g. the server wants a different field name),
 *		adjust the Conv's quirk flags and return 1 to
 *		have the request rebuilt and resent once;
 *		return 0 to let the error propagate.
 */
typedef struct Provider Provider;
struct Provider {
	char *name;
	char *apiurl;
	char *modelsurl;
	int (*headers)(int fd, char *apikey);
	Json* (*buildreq)(Conv*);
	Reply* (*readstream)(Conv*, Biobuf *bp, Usage*,
		void (*cb)(char*, void*), void *aux);
	int (*quirk)(Conv*, char *err);
};

/* claude.c internals shared with provider implementations */
Tooldef*	tooldef(int i);		/* i'th tool, nil past the end */
Tooldef*	findtool(char *name);	/* nil if unknown */
void	parseinput(ToolCall *tc, Tooldef *td, Json *input);
int	blankstr(char *s);
Json*	toolschema(Tooldef *td);	/* JSON schema for td's parameters */
void	toolfree(ToolCall *t);
void	replyfree(Reply *r);
/*
 * Build the repaired neutral {role, content} messages array
 * from a Conv (see the doc comment in claude.c).  Anthropic
 * uses it as-is; OpenAI translates each entry.  Caller frees
 * with jsonfree.
 */
Json*	neutralmessages(Conv *c);

/* openai.c */
int	openaiheaders(int fd, char *apikey);
Json*	openaibuildreq(Conv *c);
Reply*	openaireadstream(Conv *c, Biobuf *bp, Usage *usage,
		void (*cb)(char*, void*), void *aux);
int	openaiquirk(Conv *c, char *err);
