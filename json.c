#include <u.h>
#include <libc.h>
#include "json.h"
#include "claude.h"

static Json*	parseval(char**);
static int	parseu4(char*, Rune*);
static int	fmtjstr(Fmt*, char*);
static int	isdelim(int);

static char*
skip(char *p)
{
	while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static Json*
mkjson(int type)
{
	Json *j;

	j = emallocz(sizeof *j, 1);
	j->type = type;
	return j;
}

static int
hexval(int c)
{
	if('0' <= c && c <= '9')
		return c - '0';
	if('a' <= c && c <= 'f')
		return c - 'a' + 10;
	if('A' <= c && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
parseu4(char *p, Rune *rp)
{
	int i, h;
	Rune r;

	r = 0;
	for(i = 0; i < 4; i++){
		if(p[i] == '\0')
			return -1;
		h = hexval(p[i]);
		if(h < 0)
			return -1;
		r = (r << 4) | h;
	}
	*rp = r;
	return 0;
}

static char*
parsestr(char **pp)
{
	char *p, *q, *w, *s;
	Rune r, r2;

	p = skip(*pp);
	if(*p != '"'){
		werrstr("expected '\"'");
		return nil;
	}
	p++;

	/*
	 * Safe upper bound: decoded output cannot exceed the remaining
	 * bytes of input starting at p.
	 */
	s = emalloc(strlen(p) + 1);

	w = s;
	for(q = p; *q != '\0' && *q != '"'; ){
		if((uchar)*q < 0x20){
			free(s);
			werrstr("unescaped control character in string");
			return nil;
		}
		if(*q == '\\'){
			q++;
			if(*q == '\0'){
				free(s);
				werrstr("unterminated escape");
				return nil;
			}
			switch(*q){
			case '"':
			case '\\':
			case '/':
				*w++ = *q;
				q++;
				break;
			case 'b':
				*w++ = '\b';
				q++;
				break;
			case 'f':
				*w++ = '\f';
				q++;
				break;
			case 'n':
				*w++ = '\n';
				q++;
				break;
			case 'r':
				*w++ = '\r';
				q++;
				break;
			case 't':
				*w++ = '\t';
				q++;
				break;
			case 'u':
				if(parseu4(q+1, &r) < 0){
					free(s);
					werrstr("bad \\u escape");
					return nil;
				}
				q += 5;	/* skip 'u' and 4 hex digits */

				if(0xD800 <= r && r <= 0xDBFF){
					if(q[0] != '\\' || q[1] != 'u' ||
					   parseu4(q+2, &r2) < 0 ||
					   r2 < 0xDC00 || r2 > 0xDFFF){
						free(s);
						werrstr("bad surrogate pair");
						return nil;
					}
					r = 0x10000 + ((r - 0xD800) << 10) + (r2 - 0xDC00);
					q += 6;	/* skip \uXXXX of low surrogate */
				}else if(0xDC00 <= r && r <= 0xDFFF){
					free(s);
					werrstr("lone low surrogate");
					return nil;
				}
				w += runetochar(w, &r);
				break;
			default:
				free(s);
				werrstr("bad escape");
				return nil;
			}
		}else
			*w++ = *q++;
	}

	if(*q != '"'){
		free(s);
		werrstr("unterminated string");
		return nil;
	}

	*w = '\0';
	*pp = q + 1;
	return s;
}

static Json*
parsenum(char **pp)
{
	char *p, *e, *buf;
	Json *j;
	int isfloat;

	p = skip(*pp);
	isfloat = 0;
	e = p;

	if(*e == '-')
		e++;

	if(*e == '0'){
		e++;
		if(*e >= '0' && *e <= '9'){
			werrstr("bad number");
			return nil;
		}
	}else if(*e >= '1' && *e <= '9'){
		while(*e >= '0' && *e <= '9')
			e++;
	}else{
		werrstr("bad number");
		return nil;
	}

	if(*e == '.'){
		isfloat = 1;
		e++;
		if(!(*e >= '0' && *e <= '9')){
			werrstr("bad number");
			return nil;
		}
		while(*e >= '0' && *e <= '9')
			e++;
	}

	if(*e == 'e' || *e == 'E'){
		isfloat = 1;
		e++;
		if(*e == '+' || *e == '-')
			e++;
		if(!(*e >= '0' && *e <= '9')){
			werrstr("bad number");
			return nil;
		}
		while(*e >= '0' && *e <= '9')
			e++;
	}

	if(!isdelim(*e)){
		werrstr("bad number");
		return nil;
	}

	buf = emalloc(e - p + 1);
	memmove(buf, p, e - p);
	buf[e - p] = '\0';

	if(isfloat){
		j = mkjson(Jreal);
		j->fval = strtod(buf, nil);
	} else {
		j = mkjson(Jint);
		j->ival = strtoll(buf, nil, 10);
	}
	free(buf);

	*pp = e;
	return j;
}

void
jgrow(Json *j)
{
	if(j->nitem >= j->aitem){
		j->aitem = j->aitem ? j->aitem * 2 : 8;
		j->items = erealloc(j->items, j->aitem * sizeof(Json*));
		if(j->type == Jobject)
			j->names = erealloc(j->names, j->aitem * sizeof(char*));
	}
}

static Json*
parsearray(char **pp)
{
	Json *j, *v;
	char *p;

	p = skip(*pp);
	if(*p != '['){
		werrstr("expected '['");
		return nil;
	}
	p++;
	j = mkjson(Jarray);
	p = skip(p);
	if(*p == ']'){
		*pp = p + 1;
		return j;
	}
	for(;;){
		v = parseval(&p);
		if(v == nil){
			jsonfree(j);
			return nil;
		}
		jgrow(j);
		j->items[j->nitem++] = v;
		p = skip(p);
		if(*p == ']'){
			*pp = p + 1;
			return j;
		}
		if(*p != ','){
			werrstr("expected ',' or ']' in array");
			jsonfree(j);
			return nil;
		}
		p++;
	}
}

static Json*
parseobject(char **pp)
{
	Json *j, *v;
	char *p, *name;

	p = skip(*pp);
	if(*p != '{'){
		werrstr("expected '{'");
		return nil;
	}
	p++;
	j = mkjson(Jobject);
	p = skip(p);
	if(*p == '}'){
		*pp = p + 1;
		return j;
	}
	for(;;){
		name = parsestr(&p);
		if(name == nil){
			jsonfree(j);
			return nil;
		}
		p = skip(p);
		if(*p != ':'){
			free(name);
			jsonfree(j);
			werrstr("expected ':'");
			return nil;
		}
		p++;
		v = parseval(&p);
		if(v == nil){
			free(name);
			jsonfree(j);
			return nil;
		}
		jgrow(j);
		j->names[j->nitem] = name;
		j->items[j->nitem] = v;
		j->nitem++;
		p = skip(p);
		if(*p == '}'){
			*pp = p + 1;
			return j;
		}
		if(*p != ','){
			werrstr("expected ',' or '}' in object");
			jsonfree(j);
			return nil;
		}
		p++;
	}
}

static int
isdelim(int c)
{
	return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == ',' || c == ']' || c == '}';
}

static Json*
parseval(char **pp)
{
	char *p;
	Json *j;

	p = skip(*pp);
	switch(*p){
	case '"':
		j = mkjson(Jstring);
		j->str = parsestr(pp);
		if(j->str == nil){
			free(j);
			return nil;
		}
		return j;
	case '{':
		return parseobject(pp);
	case '[':
		return parsearray(pp);
	case 't':
		if(strncmp(p, "true", 4) == 0 && isdelim(p[4])){
			*pp = p + 4;
			j = mkjson(Jbool);
			j->ival = 1;
			return j;
		}
		break;
	case 'f':
		if(strncmp(p, "false", 5) == 0 && isdelim(p[5])){
			*pp = p + 5;
			j = mkjson(Jbool);
			j->ival = 0;
			return j;
		}
		break;
	case 'n':
		if(strncmp(p, "null", 4) == 0 && isdelim(p[4])){
			*pp = p + 4;
			return mkjson(Jnull);
		}
		break;
	default:
		if(*p == '-' || (*p >= '0' && *p <= '9'))
			return parsenum(pp);
		break;
	}
	werrstr("unexpected character '%c'", *p);
	return nil;
}

Json*
jsonparse(char *s)
{
	char *p;
	Json *j;

	p = s;
	j = parseval(&p);
	if(j == nil)
		return nil;
	p = skip(p);
	if(*p != '\0'){
		jsonfree(j);
		werrstr("trailing garbage");
		return nil;
	}
	return j;
}

/* access helpers */

Json*
jget(Json *j, char *name)
{
	int i;

	if(j == nil || j->type != Jobject)
		return nil;
	for(i = 0; i < j->nitem; i++)
		if(strcmp(j->names[i], name) == 0)
			return j->items[i];
	return nil;
}

char*
jstr(Json *j, char *name)
{
	Json *v;

	v = jget(j, name);
	if(v == nil || v->type != Jstring)
		return nil;
	return v->str;
}

vlong
jint(Json *j, char *name)
{
	Json *v;

	v = jget(j, name);
	if(v == nil)
		return 0;
	if(v->type == Jint)
		return v->ival;
	/*
	 * Tolerate numbers that arrive as reals (e.g. "123.0"):
	 * JSON has only one number type, so a serializer is free
	 * to add a fraction part to an integral value.
	 */
	if(v->type == Jreal)
		return (vlong)v->fval;
	return 0;
}

Json*
jidx(Json *j, int i)
{
	if(j == nil || j->type != Jarray)
		return nil;
	if(i < 0 || i >= j->nitem)
		return nil;
	return j->items[i];
}

/* construction */

Json*
jbool(int b)
{
	Json *j;

	j = mkjson(Jbool);
	j->ival = b != 0;
	return j;
}

Json*
jstring(char *s)
{
	Json *j;

	j = mkjson(Jstring);
	j->str = estrdup(s);
	return j;
}

Json*
jintval(vlong v)
{
	Json *j;

	j = mkjson(Jint);
	j->ival = v;
	return j;
}

Json*
jarray(void)
{
	return mkjson(Jarray);
}

Json*
jobject(void)
{
	return mkjson(Jobject);
}

void
jappend(Json *arr, Json *val)
{
	if(arr->type != Jarray)
		sysfatal("jappend on non-array");
	jgrow(arr);
	arr->items[arr->nitem++] = val;
}

void
jset(Json *obj, char *name, Json *val)
{
	int i;

	if(obj->type != Jobject)
		sysfatal("jset on non-object");

	/* overwrite existing key */
	for(i = 0; i < obj->nitem; i++){
		if(strcmp(obj->names[i], name) == 0){
			jsonfree(obj->items[i]);
			obj->items[i] = val;
			return;
		}
	}
	jgrow(obj);
	obj->names[obj->nitem] = estrdup(name);
	obj->items[obj->nitem] = val;
	obj->nitem++;
}

/* JSON serialization using Fmt */

/*
 * Emit s as a JSON string literal.  Runs of plain ASCII go
 * out in one piece; quotes, backslashes, and control
 * characters are escaped; multibyte UTF-8 is decoded and
 * re-emitted one rune at a time.
 *
 * The ASCII-only batching is a correctness requirement, not
 * an optimization: the precision of %.*s counts runes, not
 * bytes (see fmtstrcpy in the fmt library), so handing it a
 * byte count is only safe when every byte in the run encodes
 * exactly one rune.  Mixing multibyte text into the batch
 * made fmt copy runes past the end of the run, emitting the
 * very quote/backslash/control character the scanner stopped
 * at -- raw and unescaped -- followed by a duplicated tail.
 * That corrupted both request bodies and stored rawjson
 * snapshots (which then failed to reparse, silently dropping
 * tool_use/tool_result blocks and wedging conversations).
 *
 * Re-encoding runes through chartorune/fmtrune also replaces
 * invalid UTF-8 byte sequences with U+FFFD; the API rejects
 * request bodies that are not valid UTF-8, so raw bytes from
 * e.g. a binary file read must never pass through untouched.
 */
static int
fmtjstr(Fmt *f, char *s)
{
	char *p, *q, *esc;
	Rune r;

	fmtrune(f, '"');
	for(p = s; *p != '\0'; p = q){
		for(q = p; (uchar)*q >= 0x20 && (uchar)*q < Runeself
		&& *q != '"' && *q != '\\'; q++)
			;
		if(q > p)
			fmtprint(f, "%.*s", (int)(q - p), p);
		if(*q == '\0')
			break;
		if((uchar)*q >= Runeself){
			q += chartorune(&r, q);
			fmtrune(f, r);
			continue;
		}
		switch(*q++){
		case '"':	esc = "\\\""; break;
		case '\\':	esc = "\\\\"; break;
		case '\b':	esc = "\\b"; break;
		case '\f':	esc = "\\f"; break;
		case '\n':	esc = "\\n"; break;
		case '\r':	esc = "\\r"; break;
		case '\t':	esc = "\\t"; break;
		default:
			fmtprint(f, "\\u%04x", (uchar)q[-1]);
			continue;
		}
		fmtprint(f, "%s", esc);
	}
	return fmtrune(f, '"');
}

static int
jsonfmt(Json *j, Fmt *f)
{
	int i;

	if(j == nil)
		return fmtprint(f, "null");

	switch(j->type){
	case Jnull:
		return fmtprint(f, "null");
	case Jbool:
		return fmtprint(f, j->ival ? "true" : "false");
	case Jint:
		return fmtprint(f, "%lld", j->ival);
	case Jreal:
		return fmtprint(f, "%g", j->fval);
	case Jstring:
		return fmtjstr(f, j->str);
	case Jarray:
		fmtrune(f, '[');
		for(i = 0; i < j->nitem; i++){
			if(i > 0)
				fmtrune(f, ',');
			jsonfmt(j->items[i], f);
		}
		return fmtrune(f, ']');
	case Jobject:
		fmtrune(f, '{');
		for(i = 0; i < j->nitem; i++){
			if(i > 0)
				fmtrune(f, ',');
			fmtjstr(f, j->names[i]);
			fmtrune(f, ':');
			jsonfmt(j->items[i], f);
		}
		return fmtrune(f, '}');
	}
	return fmtprint(f, "null");
}

char*
jsonstr(Json *j)
{
	Fmt f;

	fmtstrinit(&f);
	jsonfmt(j, &f);
	return fmtstrflush(&f);
}

void
jsonfree(Json *j)
{
	int i;

	if(j == nil)
		return;
	switch(j->type){
	case Jstring:
		free(j->str);
		break;
	case Jarray:
		for(i = 0; i < j->nitem; i++)
			jsonfree(j->items[i]);
		free(j->items);
		break;
	case Jobject:
		for(i = 0; i < j->nitem; i++){
			free(j->names[i]);
			jsonfree(j->items[i]);
		}
		free(j->items);
		free(j->names);
		break;
	}
	free(j);
}
