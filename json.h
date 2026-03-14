typedef struct Json Json;

enum {
	Jnull,
	Jbool,
	Jint,
	Jreal,
	Jstring,
	Jarray,
	Jobject,
};

struct Json {
	int type;
	/* Jbool, Jint */
	vlong ival;
	/* Jreal */
	double fval;
	/* Jstring */
	char *str;
	/* Jarray, Jobject */
	Json **items;
	char **names;	/* Jobject only */
	int nitem;
	int aitem;	/* allocated slots */
};

/* parsing */
Json*	jsonparse(char *s);

/* access */
Json*	jget(Json *j, char *name);
char*	jstr(Json *j, char *name);
vlong	jint(Json *j, char *name);
Json*	jidx(Json *j, int i);

/* construction */
Json*	jnull(void);
Json*	jbool(int b);
Json*	jstring(char *s);
Json*	jstringn(char *s, int n);
Json*	jintval(vlong v);
Json*	jreal(double v);
Json*	jarray(void);
Json*	jobject(void);
void	jappend(Json *arr, Json *val);
void	jset(Json *obj, char *name, Json *val);

/* output */
char*	jsonstr(Json *j);

/* free */
void	jsonfree(Json *j);
