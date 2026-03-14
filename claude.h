typedef struct Msg Msg;
typedef struct Conv Conv;
typedef struct ModelInfo ModelInfo;
typedef struct Usage Usage;

enum {
	Muser,
	Massistant,
};

struct Msg {
	int role;
	char *text;
	Msg *next;
};

struct Conv {
	Msg *msgs;
	Msg *tail;
	char *model;
	char *apikey;
	int maxtokens;
	char *sysprompt;
	char *webdir;	/* webfs clone dir, e.g. /mnt/web/N */
};

struct ModelInfo {
	char *id;		/* API model identifier */
	char *display_name;	/* human-readable name from API */
	int max_output_tokens;	/* maximum output tokens for this model */
};

struct Usage {
	int input_tokens;
	int output_tokens;
	char *stop_reason; /* "end_turn", "max_tokens", etc */
};

Conv*	convnew(char *apikey, char *model, int maxtokens, char *sysprompt);
void	convfree(Conv *c);
Msg*	msgnew(int role, char *text);
void	convappend(Conv *c, Msg *m);
char*	claudesend(Conv *c, Usage *usage);

int	convcount(Conv *c);
long	convsize(Conv *c);
int	fetchmodels(char *apikey, ModelInfo **out);
char*	readfd(int fd);
char*	readfile(int fd);

typedef struct Action Action;

enum {
	Acreate,
	Adelete,
	Apatch,
};

struct Action {
	int type;
	char *path;
	char *body;	/* file contents for create, diff for patch, nil for delete */
	Action *next;
};

Action*	parseactions(char *reply);
char*	stripactions(char *reply);
void	showaction(Action *a, int n);
int	applyaction(Action *a);
void	freeactions(Action *a);
void	mkparents(char *path);
