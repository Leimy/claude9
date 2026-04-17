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
	int type;		/* Acreate, Apatch, Adelete */
	char *path;
	char *body;
	char *result;		/* result text after execution */
	ToolCall *next;
};

/* result of claudechat - may contain text and/or tool calls */
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
typedef struct Action Action;

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

struct Action {
	int type;		/* "create", "patch", "delete" */
	char *path;
	char *body;
	Action *next;
};

/* claude.c */
Conv*	convnew(char *apikey, char *model, int maxtokens, char *sysprompt);
void	convfree(Conv *c);
int	convcount(Conv *c);
long	convsize(Conv *c);
Msg*	msgnew(int role, char *text);
Msg*	msgnewraw(int role, char *text, char *rawjson);
void	convappend(Conv *c, Msg *m);
char*	claudesend(Conv *c, Usage *usage);
Reply*	claudechat(Conv *c, Usage *usage);
char*	claudeconverse(Conv *c, Usage *usage);
void	replyfree(Reply *r);
void	toolfree(ToolCall *t);
char*	readfile(int fd);
void	mkparents(char *path);
int	fetchmodels(char *apikey, ModelInfo **out);

/* action.c */
Action*	parseactions(char *text);
void	freeactions(Action *a);
void	showaction(Action *a, int n);
int	applyaction(Action *a);
char*	stripactions(char *text);
/*
 * Apply a unified diff to the file at path using the in-tree
 * fuzzy applier. Returns a malloc'd status string describing
 * success ("patched foo.c (N hunks)") or failure ("error: ...").
 * Never returns nil.
 */
char*	applydiff(char *path, char *diff);
