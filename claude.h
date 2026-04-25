#pragma once

enum {
	Acreate,
	Apatch,
	Aread,
	Alist,
	Adelete,
};

enum {
	Muser,
	Massistant,
};

/* tool call from Claude */
typedef struct ToolCall ToolCall;
struct ToolCall {
	char *id;		/* tool_use_id */
	int type;		/* Acreate, Apatch, Adelete, Aread, Alist */
	char *path;		/* file path */
	char *body;		/* contents / diff */
	char *result;		/* result text after execution */
	ToolCall *next;
};

/*
 * Reply assembled from a single API round (with or without
 * streaming).  May contain text, tool_use blocks, and a raw
 * JSON snapshot of the assistant's content array suitable for
 * appending back into a follow-up request.
 */
typedef struct Reply Reply;
struct Reply {
	char *text;		/* concatenated text blocks */
	char *rawjson;		/* raw JSON content array string */
	ToolCall *tools;	/* linked list of tool calls */
	int stopped;		/* 1 if stop_reason was end_turn */
};

typedef struct Msg Msg;
typedef struct Conv Conv;
typedef struct Usage Usage;
typedef struct ModelInfo ModelInfo;

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
	char *sysprompt;
	char *webdir;
	Msg *msgs;
	Msg *tail;
};

struct Usage {
	int input_tokens;
	int output_tokens;
	char *stop_reason;
};

struct ModelInfo {
	char *id;
	char *display_name;
	int max_output_tokens;
};

/* claude.c */
Conv*	convnew(char *apikey, char *model, int maxtokens, char *sysprompt);
void	convfree(Conv *c);
int	convcount(Conv *c);
long	convsize(Conv *c);
Msg*	msgnew(int role, char *text);
Msg*	msgnewraw(int role, char *text, char *rawjson);
void	convappend(Conv *c, Msg *m);
/*
 * Run the full tool loop: send the conversation, execute any
 * tool calls Claude makes, send the results back, repeat until
 * a non-tool stop_reason.  Appends all rounds (assistant +
 * tool_result) to the conversation.  Returns the final
 * assistant text (caller frees) or nil on error.
 */
char*	claudeconverse(Conv *c, Usage *usage);
/*
 * Like claudeconverse, but invokes cb(chunk, aux) with each
 * incremental text delta as it arrives from the API.  Between
 * tool-use rounds, cb may be called with a short marker string
 * such as "\n[running tool: ...]\n" so the user sees progress.
 * Returns the full concatenated assistant text (caller frees),
 * same as claudeconverse.  Falls back to claudeconverse when
 * webfs is not available (e.g. plan9port with only curl); in
 * that case cb is called once with the entire final text.
 */
char*	claudeconverse_stream(Conv *c, Usage *usage,
		void (*cb)(char *chunk, void *aux), void *aux);
void	replyfree(Reply *r);
void	toolfree(ToolCall *t);
char*	readfile(int fd);
void	mkparents(char *path);
int	fetchmodels(char *apikey, ModelInfo **out);

/* patch.c */
/*
 * Apply a unified diff to the file at path using the in-tree
 * fuzzy applier. Returns a malloc'd status string describing
 * success ("patched foo.c (N hunks)") or failure ("error: ...").
 * Never returns nil.
 */
char*	applydiff(char *path, char *diff);
