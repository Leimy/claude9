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
	char *apikey;
	char *model;
	int maxtokens;
	int thinkmode;	/* Thinkoff, Thinkbudget, Thinkadaptive */
	int thinking;	/* Thinkbudget: budget tokens; 1024 <= thinking < maxtokens */
	char *effort;	/* Thinkadaptive: output_config.effort, nil = unset */
	char *sysprompt;
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
Msg*	msgnew(int role, char *text, char *rawjson);
void	convappend(Conv *c, Msg *m);
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
char*	fetchmodels(char *apikey);	/* model ids, one per line */

/* emalloc wrappers: succeed or sysfatal */
void*	emalloc(ulong n);
void*	erealloc(void *p, ulong n);
void*	emallocz(ulong n, int clr);
char*	estrdup(char *s);
char*	esmprint(char *fmt, ...);
#pragma varargck argpos esmprint 1
