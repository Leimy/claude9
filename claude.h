typedef struct Msg Msg;
typedef struct Conv Conv;

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

Conv*	convnew(char *apikey, char *model, int maxtokens, char *sysprompt);
void	convfree(Conv *c);
Msg*	msgnew(int role, char *text);
void	convappend(Conv *c, Msg *m);
char*	claudesend(Conv *c);
