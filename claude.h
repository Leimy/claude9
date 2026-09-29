#pragma once

enum {
	Muser,
	Massistant,
};

/* extended thinking modes (Conv.thinkmode) */
enum {
	Thinkoff,	/* no thinking requested */
	Thinkbudget,	/* thinking.type=enabled, budget_tokens (opus etc.) */
	Thinkadaptive,	/* thinking.type=adaptive, output_config.effort (fable) */
};

/*
 * openai reasoning_effort quirk ladder (Conv.reasonquirk).
 * Some servers reject function tools when reasoning is in
 * effect -- whether because we sent reasoning_effort, or
 * because the server applies a DEFAULT reasoning effort for
 * the model when the field is absent (observed live: a fresh
 * Thinkoff session, field never sent, still rejected).  In
 * the latter case the only way to honor "do not set
 * reasoning_effort" is to send an explicit "none" to override
 * the server-side default; omitting the field can never fix
 * anything.  openaiquirk walks this ladder monotonically as
 * rejections come in; each state changes what openaibuildreq
 * emits.  See openaiquirk in openai.c.
 */
enum {
	Reffort,	/* normal: send effort iff Thinkadaptive */
	Romit,		/* effort value rejected with tools: omit the field */
	Rnone,		/* absence also rejected (server default reasoning):
			 * send explicit reasoning_effort "none" */
	Rdead,		/* "none" also rejected and no Responses API
			 * fallback was possible: suppress the field and
			 * let errors surface; terminal */
};

/*
 * OpenAI Responses API request-shape quirks (Conv.respquirks, a
 * bitmask advanced by responsesquirk in responses.c).  Each bit
 * turns off one optional part of the request after the server
 * rejects it, so the responses provider degrades gracefully on
 * compatible servers that implement only the core of the API.
 */
enum {
	Rqnoinclude = 1<<0,	/* server rejects store:false +
				 * include:["reasoning.encrypted_content"]:
				 * omit both (server-side state instead) */
	Rqnosummary = 1<<1,	/* server rejects reasoning.summary: omit it
				 * (no streamed thinking text) */
	Rqnoreasonitems = 1<<2,	/* server rejects replayed reasoning items
				 * or item ids: replay neither */
};

enum {
	Defmaxrounds = 20,	/* default Conv.maxrounds (see claudeconverse) */
};

/*
 * Error text for a prompt stopped by Conv.cancel.  A macro, not
 * a variable, so every producer (the stream readers, sendonce,
 * claudeconverse) and cancelerr agree on one spelling.
 */
#define Cancelmsg "prompt cancelled"

/* growable string buffer */
typedef struct Sbuf Sbuf;
struct Sbuf {
	char *s;
	int len;
	int cap;
};

typedef struct Msg Msg;
typedef struct Conv Conv;
typedef struct Usage Usage;

struct Msg {
	int role;
	char *text;
	char *rawjson;		/* raw JSON content array, nil for plain text */
	Msg *next;
};

struct Conv {
	int prov;	/* provider index (see providerlookup); default anthropic */
	char *baseurl;	/* nil/empty = provider default; else overrides the
			 * chat endpoint URL (openai-compatible servers live
			 * at arbitrary addresses) */
	char *apikey;
	char *model;
	int maxtokens;
	int oldmaxtok;	/* openai quirk: server wants legacy "max_tokens"
			 * instead of "max_completion_tokens"; set
			 * automatically when the server complains
			 * (see openaiquirk) */
	int nostreamopts;	/* openai quirk: compatible server rejects
				 * stream_options; omit it after the first
				 * such rejection (usage may be absent) */
	int reasonquirk;	/* openai quirk ladder state (Reffort,
				 * Romit, Rnone, Rdead; see the enum
				 * above): how to spell -- or not spell
				 * -- reasoning_effort so this server
				 * accepts function tools.  Tools are
				 * load-bearing, so the fix is always on
				 * the reasoning side, never dropping
				 * tools.  Advanced automatically by
				 * openaiquirk as the server complains;
				 * when no spelling works, openaiquirk
				 * moves the Conv to the responses
				 * provider instead (see switchresponses
				 * in openai.c). */
	int respquirks;	/* responses provider quirk bits (Rqnoinclude,
			 * ...; see the enum above), set automatically by
			 * responsesquirk as the server complains */
	int thinkmode;	/* Thinkoff, Thinkbudget, Thinkadaptive */
	int thinking;	/* Thinkbudget: budget tokens; 1024 <= thinking < maxtokens */
	char *effort;	/* Thinkadaptive: output_config.effort, nil = unset */
	char *advisormodel;	/* Anthropic server-side advisor; nil = disabled */
	int advisormaxuses;	/* 0 = omit/unlimited */
	int advisormaxtokens;	/* 0 = omit/provider default */
	char *advisorcache;	/* nil, "5m", or "1h" */
	int maxrounds;	/* tool-loop round cap per prompt; 0 = Defmaxrounds.
			 * Each round is one API request; a round that
			 * calls tools is followed by another.  Raise it
			 * for long multi-file tasks so the loop does not
			 * stop at the cap and need a "Continue." */
	char **searchurls;	/* URLs seen in this conversation's web_search
				 * results; web_fetch may only retrieve a
				 * URL that appears here (see rememberurl/
				 * urlsearched in claude.c); convclear revokes
				 * them with the message history */
	int nsearchurls;
	int cancel;	/* set by the server to stop the round in flight:
			 * checked by the stream readers on every line, by
			 * sendonce, by claudeconverse between rounds, and
			 * by exectool before each tool.  The server pairs
			 * it with threadint() so a read blocked in webfs
			 * returns at once instead of waiting for the next
			 * chunk.  Cleared by the server at the start of
			 * each prompt, never here. */
	char *basesys;	/* sysprompt before skills are appended */
	char *sysprompt;	/* basesys + current skills: what's actually sent */
	Msg *msgs;
	Msg *tail;
};

struct Usage {
	int input_tokens;
	int output_tokens;
	int cache_creation_input_tokens;
	int cache_read_input_tokens;
	char *stop_reason;
};

/* claude.c */
Conv*	convnew(char *apikey, char *model, int maxtokens, char *sysprompt, char *skills);
void	convfree(Conv *c);
void	convclear(Conv *c);
/*
 * Set a conversation's base system prompt and recompute the
 * effective sysprompt (base + skills, skills may be nil/empty)
 * that buildreq actually sends.  base is remembered so skills
 * can be recombined later (a system-file write, or a skills
 * reload) without needing to know or strip whatever skills
 * text was appended last time.
 */
void	convsetprompt(Conv *c, char *base, char *skills);
/* text may be nil (stored as ""); rawjson may be nil for plain text */
Msg*	msgnew(int role, char *text, char *rawjson);
void	convappend(Conv *c, Msg *m);
/*
 * Drop whole leading exchanges from the front of the
 * conversation, keeping at least the most recent keep
 * exchanges, so the surviving history fits a smaller context.
 *
 * An "exchange" begins at a real user turn (a user message
 * with rawjson==nil: an actual prompt or a "Continue.", not a
 * tool_results message) and runs up to but not including the
 * next real user turn.  Cutting only on exchange boundaries
 * keeps every tool_use paired with its tool_result and leaves
 * the history starting on a user turn, which is what the API
 * requires.
 *
 * keep must be >= 1; the most recent exchange is never
 * dropped.  Returns the number of messages removed (0 if the
 * conversation already had keep or fewer exchanges).
 */
int	convcompact(Conv *c, int keep);
/* Count the real user turns (exchanges) in the conversation. */
int	convnexchanges(Conv *c);
/* Approximate input size in bytes (text + rawjson of every message). */
long	convinputbytes(Conv *c);
/*
 * True if an error string from claudeconverse indicates the
 * model's input context window was exceeded ("prompt is too
 * long").  Such an error wedges the session at its current
 * size -- every resend fails identically -- until history is
 * dropped, so callers should react by compacting or clearing
 * rather than just reporting it.
 */
int	overlimiterr(char *err);
/*
 * The user-facing text for such an error: the raw error plus the
 * remedy ("echo compact > ctl", "echo clear > ctl").  Malloc'd;
 * caller frees.  Lives in claude.c so tests.c can exercise it.
 */
char*	overlimitmsg(char *err);
/*
 * True if an error string from claudeconverse is the specific
 * "tool loop limit reached" condition: the tool loop hit its
 * per-prompt round cap while the model was still calling tools.
 * The conversation is left well-formed and resumable (it ends
 * on a tool_results user turn), so this is recoverable
 * (auto-continue can send another "Continue."), unlike a real
 * API failure.  The sibling "tool/advisor loop limit reached"
 * wording, emitted when the cap fell on a server-side advisor
 * pause_turn round, is deliberately NOT matched: that
 * conversation ends on an assistant turn that must be replayed
 * unchanged, and a "Continue." would violate the pause
 * protocol.
 */
int	toollimiterr(char *err);
/*
 * True if an error string from claudeconverse says the prompt
 * was stopped by Conv.cancel (Cancelmsg).  Not a failure: the
 * conversation is well-formed and resumable, and callers must
 * not auto-continue it.
 */
int	cancelerr(char *err);
/*
 * Run the full tool loop: send the conversation, execute any
 * tool calls Claude makes, send the results back, repeat until
 * a non-tool stop_reason.  Appends all rounds (assistant +
 * tool_result) to the conversation.  When cb is non-nil, invokes
 * cb(chunk, aux) with each incremental text delta as it arrives
 * from the API.  Between tool-use rounds, cb may be called with
 * a short marker string such as "\n[running tool: ...]\n".
 * Returns the full concatenated assistant text (caller frees),
 * or nil on error.
 *
 * If errp is non-nil, *errp is set to a malloc'd error string
 * when something went wrong, even if partial text is returned
 * (e.g. an API failure after several successful tool rounds).
 * *errp is set to nil on full success.  This is how callers
 * distinguish "complete answer" from "answer truncated by an
 * error mid-loop".
 */
char*	claudeconverse(Conv *c, Usage *usage,
		void (*cb)(char *chunk, void *aux), void *aux, char **errp);
void	sbappend(Sbuf *b, char *s, int n);
char*	readfile(int fd);
char*	fetchmodels(int prov, char *apikey);	/* model ids, one per line */
/*
 * Providers are named wire-format implementations (request
 * assembly, auth headers, stream parsing) private to claude.c;
 * the public handle is a small index so no provider types leak
 * into this header (kencc type signatures require complete
 * types on both sides of a link).  providerlookup maps a name
 * ("anthropic", ...) to an index, -1 if unknown.  convnew
 * defaults every conversation to the anthropic provider;
 * callers may reassign Conv.prov between prompts.
 */
int	providerlookup(char *name);
char*	providername(int prov);
int	providercount(void);	/* valid indexes are 0..providercount()-1 */
/*
 * True if the provider has a model-list endpoint of its own.
 * The responses provider is a second wire format for the same
 * OpenAI models the openai provider already lists, so it has
 * none; a caller enumerating providers for a models listing
 * should skip it rather than print the same ids twice.
 */
int	providerhasmodels(int prov);

/* emalloc wrappers: succeed or sysfatal */
void*	emalloc(ulong n);
void*	erealloc(void *p, ulong n);
void*	emallocz(ulong n, int clr);
char*	estrdup(char *s);
char*	esmprint(char *fmt, ...);
#pragma varargck argpos esmprint 1
