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
	Rdead,		/* even "none" rejected: suppress the field and
			 * let errors surface; terminal */
};

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
	int reasonquirk;	/* openai quirk ladder state (Reffort,
				 * Romit, Rnone, Rdead; see the enum
				 * above): how to spell -- or not spell
				 * -- reasoning_effort so this server
				 * accepts function tools.  Tools are
				 * load-bearing, so the fix is always on
				 * the reasoning side, never dropping
				 * tools.  Advanced automatically by
				 * openaiquirk as the server complains. */
	int thinkmode;	/* Thinkoff, Thinkbudget, Thinkadaptive */
	int thinking;	/* Thinkbudget: budget tokens; 1024 <= thinking < maxtokens */
	char *effort;	/* Thinkadaptive: output_config.effort, nil = unset */
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
 * True if an error string from claudeconverse is the specific
 * "tool loop limit reached" condition: the tool loop hit its
 * per-prompt round cap while the model was still calling tools.
 * The conversation is left well-formed and resumable, so this
 * is recoverable (auto-continue can send another "Continue."),
 * unlike a real API failure.
 */
int	toollimiterr(char *err);
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

/* emalloc wrappers: succeed or sysfatal */
void*	emalloc(ulong n);
void*	erealloc(void *p, ulong n);
void*	emallocz(ulong n, int clr);
char*	estrdup(char *s);
char*	esmprint(char *fmt, ...);
#pragma varargck argpos esmprint 1
