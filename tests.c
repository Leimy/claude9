/*
 * Unit tests for claude9's pure logic: the JSON library, the
 * conversation-repair passes, request assembly, history
 * compaction, and the file-editing tool primitives.  Nothing
 * here talks to the network or serves 9P, so the tests run
 * anywhere, any time.
 *
 * Build with "mk tests", run with ./tests.  Each failure prints
 * one FAIL line; the exit status is "fail" if anything failed,
 * empty otherwise.
 *
 * This file #includes claude.c so the tests can reach its
 * static internals (blankstr, striptextblocks, repairtooluse,
 * toolreplace, pathhash, ...) without exporting them from the
 * production build.  Link against json.$O only -- linking
 * claude.$O too would duplicate every symbol (see the mkfile's
 * tests target).
 *
 * Some tests deliberately drive the repair passes through their
 * recovery paths, which print "claude: repaired ..." and
 * "claude: dropped ..." diagnostics on stderr; those lines are
 * expected.  The verdict is the summary line at the end.
 */
#include "claude.c"

static int nrun;
static int nfail;

static char* writetmpsse(char*);

static void
ok(int cond, char *name)
{
	nrun++;
	if(!cond){
		fprint(2, "FAIL: %s\n", name);
		nfail++;
	}
}

static void
okstr(char *got, char *want, char *name)
{
	nrun++;
	if(got == nil || strcmp(got, want) != 0){
		fprint(2, "FAIL: %s: got \"%s\" want \"%s\"\n",
			name, got != nil ? got : "(nil)", want);
		nfail++;
	}
}

/* --- small builders for content blocks and messages --- */

static Json*
textblock(char *s)
{
	Json *b;

	b = jobject();
	jset(b, "type", jstring("text"));
	jset(b, "text", jstring(s));
	return b;
}

static Json*
tooluseblock(char *id)
{
	Json *b;

	b = jobject();
	jset(b, "type", jstring("tool_use"));
	jset(b, "id", jstring(id));
	jset(b, "name", jstring("read_file"));
	jset(b, "input", jobject());
	return b;
}

static Json*
toolresultblock(char *id)
{
	Json *b;

	b = jobject();
	jset(b, "type", jstring("tool_result"));
	jset(b, "tool_use_id", jstring(id));
	jset(b, "content", jstring("ok"));
	return b;
}

static Json*
mkmsg(char *role, Json *content)
{
	Json *m;

	m = jobject();
	jset(m, "role", jstring(role));
	jset(m, "content", content);
	return m;
}

/* --- json.c: parsing --- */

static void
tjsonparse(void)
{
	Json *j, *b;
	char cbuf[4];

	j = jsonparse("{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":-2.5}");
	ok(j != nil, "parse object");
	if(j != nil){
		ok(jint(j, "a") == 1, "jint");
		b = jget(j, "b");
		ok(b != nil && b->type == Jarray && b->nitem == 4, "array field");
		if(b != nil && b->nitem == 4){
			ok(jidx(b, 0)->type == Jbool && jidx(b, 0)->ival == 1, "true");
			ok(jidx(b, 1)->type == Jbool && jidx(b, 1)->ival == 0, "false");
			ok(jidx(b, 2)->type == Jnull, "null");
			okstr(jidx(b, 3)->str, "x", "string element");
			ok(jidx(b, 4) == nil, "jidx out of range");
		}
		ok(jget(j, "c") != nil && jget(j, "c")->type == Jreal, "negative real");
		ok(jget(j, "nosuch") == nil, "jget missing key");
		ok(jstr(j, "a") == nil, "jstr on non-string");
		jsonfree(j);
	}

	/* jint tolerates integral reals (serializer freedom) */
	j = jsonparse("{\"n\":123.0}");
	ok(j != nil && jint(j, "n") == 123, "jint on real");
	jsonfree(j);

	j = jsonparse("1e3");
	ok(j != nil && j->type == Jreal, "exponent parses as real");
	jsonfree(j);

	j = jsonparse("-0");
	ok(j != nil && j->type == Jint && j->ival == 0, "minus zero");
	jsonfree(j);

	/* rejections */
	ok(jsonparse("01") == nil, "leading zero rejected");
	ok(jsonparse("1.") == nil, "bare decimal point rejected");
	ok(jsonparse("1e") == nil, "bare exponent rejected");
	ok(jsonparse("-") == nil, "bare minus rejected");
	ok(jsonparse("1 x") == nil, "trailing garbage rejected");
	ok(jsonparse("[1,]") == nil, "dangling comma rejected");
	ok(jsonparse("{\"a\":}") == nil, "missing value rejected");
	ok(jsonparse("tru") == nil, "truncated keyword rejected");
	ok(jsonparse("\"abc") == nil, "unterminated string rejected");

	/* raw control character inside a string literal */
	cbuf[0] = '"';
	cbuf[1] = 0x01;
	cbuf[2] = '"';
	cbuf[3] = '\0';
	ok(jsonparse(cbuf) == nil, "raw control char in string rejected");
}

/* --- json.c: string escaping, both directions --- */

static void
tjsonstring(void)
{
	Json *j, *j2;
	char *s, *p;
	char buf[8], want[8];
	Rune r;
	int n, nosurr;

	/* decode escapes, then re-encode */
	j = jsonparse("\"a\\nb\\t\\\"q\\\\c\\u0041\"");
	ok(j != nil, "parse escaped string");
	if(j != nil){
		okstr(j->str, "a\nb\t\"q\\cA", "escape decoding");
		s = jsonstr(j);
		okstr(s, "\"a\\nb\\t\\\"q\\\\cA\"", "escape re-encoding");
		free(s);
		jsonfree(j);
	}

	/* control characters go out as \u00xx */
	buf[0] = 0x01;
	buf[1] = '\0';
	j = jstring(buf);
	s = jsonstr(j);
	okstr(s, "\"\\u0001\"", "control char escaped on output");
	free(s);
	jsonfree(j);

	/* surrogate pair combines into one astral rune */
	j = jsonparse("\"\\ud83d\\ude00\"");
	ok(j != nil, "surrogate pair parses");
	if(j != nil){
		chartorune(&r, j->str);
		ok(r == 0x1F600, "surrogate pair combines");
		s = jsonstr(j);
		j2 = jsonparse(s);
		ok(j2 != nil && strcmp(j2->str, j->str) == 0, "astral round trip");
		free(s);
		jsonfree(j2);
		jsonfree(j);
	}

	/* lone surrogates become U+FFFD, not a parse failure */
	j = jsonparse("\"\\ud800x\"");
	ok(j != nil, "lone high surrogate tolerated");
	if(j != nil){
		chartorune(&r, j->str);
		ok(r == Runeerror, "lone high surrogate -> U+FFFD");
		ok(j->str[strlen(j->str)-1] == 'x', "text after surrogate kept");
		jsonfree(j);
	}
	j = jsonparse("\"\\udc00\"");
	ok(j != nil, "lone low surrogate tolerated");
	if(j != nil){
		chartorune(&r, j->str);
		ok(r == Runeerror, "lone low surrogate -> U+FFFD");
		jsonfree(j);
	}

	/* invalid UTF-8 bytes are folded to U+FFFD on output */
	buf[0] = (char)0xFF;
	buf[1] = '\0';
	j = jstring(buf);
	s = jsonstr(j);
	r = Runeerror;
	want[0] = '"';
	n = runetochar(want+1, &r);
	want[1+n] = '"';
	want[2+n] = '\0';
	okstr(s, want, "invalid utf-8 -> U+FFFD on output");
	free(s);
	jsonfree(j);

	/*
	 * CESU-8 surrogate bytes must never survive serialization:
	 * whatever chartorune makes of them, the output must reparse
	 * cleanly and contain no surrogate code points (the API
	 * rejects request bodies that do).
	 */
	buf[0] = (char)0xED;
	buf[1] = (char)0xA0;
	buf[2] = (char)0xBD;
	buf[3] = '\0';
	j = jstring(buf);
	s = jsonstr(j);
	j2 = jsonparse(s);
	ok(j2 != nil, "cesu-8 output reparses");
	if(j2 != nil){
		nosurr = 1;
		for(p = j2->str; *p != '\0'; p += chartorune(&r, p))
			if(0xD800 <= r && r <= 0xDFFF)
				nosurr = 0;
		ok(nosurr, "no surrogate escapes the serializer");
		jsonfree(j2);
	}
	free(s);
	jsonfree(j);
}

/* --- json.c: construction helpers --- */

static void
tjsonbuild(void)
{
	Json *o, *a;
	char *s;

	o = jobject();
	jset(o, "k", jstring("a"));
	jset(o, "k", jstring("b"));
	ok(o->nitem == 1, "jset overwrites in place");
	okstr(jstr(o, "k"), "b", "jset overwrite value");
	jsonfree(o);

	a = jarray();
	jappend(a, jintval(1));
	jappend(a, jintval(3));
	jinsert(a, 1, jintval(2));
	jinsert(a, 0, jintval(0));
	s = jsonstr(a);
	okstr(s, "[0,1,2,3]", "jinsert front and middle");
	free(s);
	jsonfree(a);
}

/* --- claude.c: blank detection and blank-block stripping --- */

static void
tblankstr(void)
{
	ok(blankstr(nil), "blankstr nil");
	ok(blankstr(""), "blankstr empty");
	ok(blankstr(" \t\r\n\v\f"), "blankstr whitespace");
	ok(!blankstr("a"), "blankstr non-blank");
	ok(!blankstr("  a  "), "blankstr embedded non-blank");
}

static void
tstrip(void)
{
	Json *c;
	int n;

	c = jarray();
	jappend(c, textblock(""));
	jappend(c, textblock(" \n\t"));
	jappend(c, tooluseblock("t1"));
	jappend(c, textblock("hi"));
	n = striptextblocks(c);
	ok(n == 2 && c->nitem == 2, "striptextblocks drops blank text");
	okstr(jstr(jidx(c, 0), "type"), "tool_use", "strip keeps tool_use");
	okstr(jstr(jidx(c, 1), "text"), "hi", "strip keeps non-blank text");
	jsonfree(c);

	ok(striptextblocks(nil) == 0, "striptextblocks nil");
}

/* --- claude.c: tool_use / tool_result repair passes --- */

static void
trepairuse(void)
{
	Json *msgs, *c;

	/* orphaned tool_use: result injected at front of next user msg */
	msgs = jarray();
	c = jarray();
	jappend(c, tooluseblock("t1"));
	jappend(msgs, mkmsg("assistant", c));
	c = jarray();
	jappend(c, textblock("next"));
	jappend(msgs, mkmsg("user", c));
	repairtooluse(msgs);
	c = jget(jidx(msgs, 1), "content");
	ok(c != nil && c->nitem == 2, "repairtooluse injects result");
	okstr(jstr(jidx(c, 0), "type"), "tool_result", "injected result first");
	okstr(jstr(jidx(c, 0), "tool_use_id"), "t1", "injected result id");
	jsonfree(msgs);

	/* orphaned tool_use at end of conversation: user msg created */
	msgs = jarray();
	c = jarray();
	jappend(c, tooluseblock("t2"));
	jappend(msgs, mkmsg("assistant", c));
	repairtooluse(msgs);
	ok(msgs->nitem == 2, "repairtooluse appends user msg");
	okstr(jstr(jidx(msgs, 1), "role"), "user", "appended msg role");
	c = jget(jidx(msgs, 1), "content");
	ok(hastoolresult(c, "t2"), "appended msg has the result");
	jsonfree(msgs);

	/* properly paired: untouched */
	msgs = jarray();
	c = jarray();
	jappend(c, tooluseblock("t3"));
	jappend(msgs, mkmsg("assistant", c));
	c = jarray();
	jappend(c, toolresultblock("t3"));
	jappend(msgs, mkmsg("user", c));
	repairtooluse(msgs);
	ok(msgs->nitem == 2, "paired tool_use: no msg added");
	c = jget(jidx(msgs, 1), "content");
	ok(c != nil && c->nitem == 1, "paired tool_use: no block added");
	jsonfree(msgs);
}

static void
trepairresults(void)
{
	Json *msgs, *c;

	/* orphaned tool_result: dropped, placeholder left behind */
	msgs = jarray();
	c = jarray();
	jappend(c, toolresultblock("zz"));
	jappend(msgs, mkmsg("user", c));
	repairtoolresults(msgs);
	c = jget(jidx(msgs, 0), "content");
	ok(c != nil && c->nitem == 1, "orphan result replaced by one block");
	okstr(jstr(jidx(c, 0), "type"), "text", "placeholder is a text block");
	jsonfree(msgs);

	/* properly paired result (plus text) survives untouched */
	msgs = jarray();
	c = jarray();
	jappend(c, tooluseblock("t4"));
	jappend(msgs, mkmsg("assistant", c));
	c = jarray();
	jappend(c, toolresultblock("t4"));
	jappend(c, textblock("hi"));
	jappend(msgs, mkmsg("user", c));
	repairtoolresults(msgs);
	c = jget(jidx(msgs, 1), "content");
	ok(c != nil && c->nitem == 2, "paired tool_result kept");
	okstr(jstr(jidx(c, 0), "type"), "tool_result", "kept result still first");
	jsonfree(msgs);
}

/* --- claude.c: request assembly --- */

static void
tbuildreq(void)
{
	Conv *c;
	Json *req, *msgs, *content;

	c = convnew("key", "test-model", 1000, "sys", nil);
	convappend(c, msgnew(Muser, "hello", nil));
	req = anthropicbuildreq(c);
	okstr(jstr(req, "model"), "test-model", "buildreq model");
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 1, "one message");
	content = jget(jidx(msgs, 0), "content");
	ok(content != nil && content->nitem == 1, "one content block");
	okstr(jstr(jidx(content, 0), "text"), "hello", "prompt text");
	jsonfree(req);

	/* consecutive same-role messages merge into one */
	convappend(c, msgnew(Muser, "again", nil));
	req = anthropicbuildreq(c);
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 1, "same-role messages merge");
	content = jget(jidx(msgs, 0), "content");
	ok(content != nil && content->nitem == 2, "merged content blocks");
	jsonfree(req);

	/* blank text becomes a placeholder, never an empty block */
	convclear(c);
	convappend(c, msgnew(Muser, "  \n", nil));
	req = anthropicbuildreq(c);
	content = jget(jidx(jget(req, "messages"), 0), "content");
	okstr(jstr(jidx(content, 0), "text"), "(no text)", "blank placeholder");
	jsonfree(req);

	/* blank text blocks inside a replayed rawjson snapshot are stripped */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "ans",
		"[{\"type\":\"text\",\"text\":\" \"},"
		"{\"type\":\"text\",\"text\":\"ans\"}]"));
	req = anthropicbuildreq(c);
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 2, "user + assistant messages");
	content = jget(jidx(msgs, 1), "content");
	ok(content != nil && content->nitem == 1, "blank block stripped from rawjson");
	okstr(jstr(jidx(content, 0), "text"), "ans", "real block survives");
	jsonfree(req);

	/* Anthropic advisor definition is appended and parameterized. */
	c->advisormodel = estrdup("claude-opus-4-8");
	c->advisormaxuses = 2;
	c->advisormaxtokens = 2048;
	c->advisorcache = estrdup("5m");
	req = anthropicbuildreq(c);
	{
		Json *ta, *at, *ca;
		ta = jget(req, "tools");
		at = jidx(ta, ta->nitem - 1);
		okstr(jstr(at, "type"), "advisor_20260301", "advisor tool type");
		okstr(jstr(at, "name"), "advisor", "advisor tool name");
		okstr(jstr(at, "model"), "claude-opus-4-8", "advisor model");
		ok(jint(at, "max_uses") == 2, "advisor max_uses");
		ok(jint(at, "max_tokens") == 2048, "advisor max_tokens");
		ca = jget(at, "caching");
		okstr(jstr(ca, "ttl"), "5m", "advisor cache ttl");
		ok(jget(at, "cache_control") != nil, "advisor is final cache breakpoint");
	}
	jsonfree(req);
	convfree(c);
}

/* --- claude.c: history accounting and compaction --- */

static void
tcompact(void)
{
	Conv *c;
	int n;

	c = convnew("k", "m", 100, "s", nil);
	convappend(c, msgnew(Muser, "abc", nil));
	convappend(c, msgnew(Massistant, "de", "[12345]"));
	ok(convinputbytes(c) == 3+2+7, "convinputbytes");
	convfree(c);

	c = convnew("k", "m", 100, "s", nil);
	convappend(c, msgnew(Muser, "u1", nil));
	convappend(c, msgnew(Massistant, "a1", nil));
	convappend(c, msgnew(Muser, "", "[1]"));	/* tool results, not an exchange */
	convappend(c, msgnew(Muser, "u2", nil));
	convappend(c, msgnew(Massistant, "a2", nil));
	convappend(c, msgnew(Muser, "u3", nil));
	convappend(c, msgnew(Massistant, "a3", nil));

	ok(convnexchanges(c) == 3, "exchange count skips tool results");
	n = convcompact(c, 5);
	ok(n == 0, "compact is a no-op when history is short");
	n = convcompact(c, 2);
	ok(n == 3, "compact drops whole first exchange");
	ok(convnexchanges(c) == 2, "two exchanges left");
	okstr(c->msgs->text, "u2", "history now starts at u2");
	n = convcompact(c, 0);	/* keep clamps to 1 */
	ok(n == 2, "keep clamps to 1");
	ok(convnexchanges(c) == 1, "one exchange left");
	okstr(c->msgs->text, "u3", "most recent exchange never dropped");
	ok(c->tail != nil && strcmp(c->tail->text, "a3") == 0, "tail unchanged");
	convfree(c);
}

/* --- claude.c: tool-call path hashing --- */

static void
tpathhash(void)
{
	ok(pathhash("") == pathhash("."), "empty path == dot");
	ok(pathhash(nil) == pathhash("."), "nil path == dot");
	ok(pathhash("/a//b") == pathhash("/a/b"), "double slash normalized");
	ok(pathhash("/a/./b") == pathhash("/a/b"), "dot element normalized");
	ok(pathhash("/a/b") != pathhash("/a/c"), "different paths differ");
}

/* --- claude.c: growable buffer --- */

static void
tsbuf(void)
{
	Sbuf b;
	int i;

	memset(&b, 0, sizeof b);
	sbappend(&b, "ab", 2);
	sbappend(&b, "cd", 2);
	okstr(b.s, "abcd", "sbappend concatenates");
	for(i = 0; i < 100; i++)
		sbappend(&b, "0123456789", 10);
	ok(b.len == 1004, "sbappend grows past initial cap");
	ok(b.s[b.len] == '\0', "sbappend keeps NUL termination");
	free(b.s);
}

/* --- claude.c: error classification --- */

static void
terrs(void)
{
	ok(overlimiterr("API error: prompt is too long: 210000 tokens > 200000 maximum"),
		"overlimiterr matches");
	ok(!overlimiterr("API error: overloaded"), "overlimiterr non-match");
	ok(!overlimiterr(nil), "overlimiterr nil");
	ok(toollimiterr("tool loop limit reached (20 rounds)"), "toollimiterr matches");
	ok(toollimiterr("tool loop limit reached (60 rounds)"), "toollimiterr matches any cap");
	ok(!toollimiterr("tool/advisor loop limit reached (20 rounds)"), "advisor cap is not auto-continuable");
	ok(!toollimiterr("some other error"), "toollimiterr non-match");
	ok(!toollimiterr(nil), "toollimiterr nil");
	/*
	 * The two wordings must stay distinguishable AND the plain
	 * one must be what claudeconverse emits for an ordinary
	 * capped tool loop -- an earlier revision emitted the
	 * advisor wording unconditionally, so toollimiterr never
	 * matched and auto-continue was silently dead.  Reproduce
	 * both esmprint formats here so a wording edit in
	 * claudeconverse that breaks the match fails this test.
	 */
	{
		char *plain, *advisor;
		plain = esmprint("tool loop limit reached (%d rounds)", Defmaxrounds);
		advisor = esmprint("tool/advisor loop limit reached (%d rounds)", Defmaxrounds);
		ok(toollimiterr(plain), "plain cap wording is auto-continuable");
		ok(!toollimiterr(advisor), "advisor cap wording is not");
		free(plain);
		free(advisor);
	}
}

/* --- claude.c: bounded model-facing file reads --- */

static void
treadlimit(void)
{
	char *path, *buf, *res;
	int fd;

	path = esmprint("/tmp/claudetest.read.%d", getpid());
	buf = emalloc(Toolreadmax + 2);
	memset(buf, 'x', Toolreadmax + 1);
	buf[Toolreadmax + 1] = '\0';

	fd = create(path, OWRITE, 0666);
	ok(fd >= 0, "read limit: create temp file");
	if(fd >= 0){
		write(fd, buf, Toolreadmax - 1);
		close(fd);
		res = toolread(path);
		ok(res != nil && strlen(res) == Toolreadmax - 1,
			"read limit: below cap is not marked truncated");
		free(res);
	}

	fd = create(path, OWRITE, 0666);
	if(fd >= 0){
		write(fd, buf, Toolreadmax);
		close(fd);
		res = toolread(path);
		ok(res != nil && strncmp(res, "warning:", 8) == 0,
			"read limit: exact cap is conservatively marked");
		free(res);
	}

	fd = create(path, OWRITE, 0666);
	if(fd >= 0){
		write(fd, buf, Toolreadmax + 1);
		close(fd);
		res = toolread(path);
		ok(res != nil && strncmp(res, "warning:", 8) == 0,
			"read limit: over cap is marked truncated");
		free(res);
	}

	remove(path);
	free(buf);
	free(path);
}

/* --- claude.c: replace_string tool --- */

static void
treplace(void)
{
	char *path, *res;
	int fd;

	path = esmprint("/tmp/claudetest.%d", getpid());
	fd = create(path, OWRITE, 0666);
	ok(fd >= 0, "create temp file");
	if(fd < 0){
		free(path);
		return;
	}
	fprint(fd, "hello world hello");
	close(fd);

	{
		Dir d;
		nulldir(&d);
		d.mode = 0751;
		dirwstat(path, &d);
	}
	res = toolreplace(path, "world", "there");
	ok(strncmp(res, "error", 5) != 0, "replace succeeds");
	free(res);
	res = toolread(path);
	okstr(res, "hello there hello", "replace result");
	free(res);
	{
		Dir *d;
		d = dirstat(path);
		ok(d != nil && (d->mode & 0777) == 0751,
			"replace preserves file mode");
		free(d);
	}

	res = toolreplace(path, "hello", "x");
	ok(strncmp(res, "error", 5) == 0, "ambiguous match rejected");
	free(res);
	res = toolread(path);
	okstr(res, "hello there hello", "file untouched after rejection");
	free(res);

	res = toolreplace(path, "zebra", "x");
	ok(strncmp(res, "error", 5) == 0, "missing match rejected");
	free(res);

	res = toolreplace(path, "", "x");
	ok(strncmp(res, "error", 5) == 0, "empty old_str rejected");
	free(res);

	res = toolreplace(path, " there", "");
	ok(strncmp(res, "error", 5) != 0, "empty new_str deletes");
	free(res);
	res = toolread(path);
	okstr(res, "hello hello", "delete result");
	free(res);

	remove(path);
	free(path);
}

/* --- claude.c: parent directory creation --- */

static void
tmkparents(void)
{
	char *base, *da, *db, *f;
	int fd;

	base = esmprint("/tmp/claudetest.d.%d", getpid());
	da = esmprint("%s/a", base);
	db = esmprint("%s/a/b", base);
	f = esmprint("%s/file", db);

	mkparents(f);
	fd = create(f, OWRITE, 0666);
	ok(fd >= 0, "mkparents creates missing directories");
	if(fd >= 0){
		close(fd);
		remove(f);
	}
	remove(db);
	remove(da);
	remove(base);
	free(f);
	free(db);
	free(da);
	free(base);
}

/* --- claude.c: man query validation (no exec on these paths) --- */

static void
ttoolman(void)
{
	char big[400], *res;
	int i;

	res = toolman(nil);
	ok(strncmp(res, "error", 5) == 0, "nil man query rejected");
	free(res);

	res = toolman("");
	ok(strncmp(res, "error", 5) == 0, "empty man query rejected");
	free(res);

	res = toolman("2 ");
	ok(strncmp(res, "error", 5) == 0, "section without page rejected");
	free(res);

	for(i = 0; i < sizeof big - 1; i++)
		big[i] = 'a';
	big[i] = '\0';
	res = toolman(big);
	ok(strncmp(res, "error", 5) == 0, "oversize man query rejected");
	free(res);
}

static void
tadvisorstream(void)
{
	char *path, *sse;
	Biobuf *bp;
	Usage u;
	Reply *r;
	Json *raw, *b;

	sse =
		"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10}}}\n"
		"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"server_tool_use\",\"id\":\"srv1\",\"name\":\"advisor\",\"input\":{}}}\n"
		"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
		"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"advisor_tool_result\",\"tool_use_id\":\"srv1\",\"content\":{\"type\":\"advisor_redacted_result\",\"encrypted_content\":\"opaque\"}}}\n"
		"data: {\"type\":\"content_block_stop\",\"index\":1}\n"
		"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"},\"usage\":{\"output_tokens\":3}}\n"
		"data: {\"type\":\"message_stop\"}\n";
	path = writetmpsse(sse);
	ok(path != nil, "advisor stream temp file");
	if(path == nil) return;
	bp = Bopen(path, OREAD);
	memset(&u, 0, sizeof u);
	r = anthropicreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);
	ok(r != nil, "advisor stream parsed");
	if(r == nil) return;
	ok(r->paused, "advisor pause_turn marked");
	ok(r->tools == nil, "advisor server tool not locally executed");
	okstr(u.stop_reason, "pause_turn", "advisor stop reason");
	raw = jsonparse(r->rawjson);
	ok(raw != nil && raw->nitem == 2, "advisor blocks preserved");
	if(raw != nil && raw->nitem == 2){
		b = jidx(raw, 1);
		okstr(jstr(b, "type"), "advisor_tool_result", "advisor result type preserved");
		okstr(jstr(jget(b, "content"), "encrypted_content"), "opaque", "advisor encrypted result preserved");
	}
	jsonfree(raw);
	replyfree(r);
	free(u.stop_reason);
}

/* --- claude.c: web_search HTML scraping helpers --- */

static void
twebsearch(void)
{
	char *s, *html;
	Searchresult results[Maxsearchresults];
	int n;

	s = urlencode("hello world/&?");
	okstr(s, "hello%20world%2F%26%3F", "urlencode escapes reserved chars");
	free(s);

	s = estrdup("a &amp; b &lt;c&gt; &quot;d&quot; &#39;e&#39; &apos;f&apos;");
	htmlunescape(s);
	okstr(s, "a & b <c> \"d\" 'e' 'f'", "htmlunescape decodes common entities");
	free(s);

	s = urldecoden("hello%20world", "hello%20world" + strlen("hello%20world"));
	okstr(s, "hello world", "urldecoden decodes percent escapes");
	free(s);

	/* DuckDuckGo redirect wrapper unwraps to the real target */
	s = resolvehref("//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpage&amp;rut=x");
	okstr(s, "https://example.com/page", "resolvehref unwraps uddg redirect");
	free(s);

	/* protocol-relative URL gets an https prefix */
	s = resolvehref("//example.org/x");
	okstr(s, "https://example.org/x", "resolvehref fixes protocol-relative URL");
	free(s);

	/*
	 * A synthetic results page: a relative nav link back to the
	 * search engine itself (filtered: not http/https after
	 * resolution), an icon anchor and a text anchor wrapping the
	 * same target (deduped, longer text wins), and a second,
	 * distinct result.
	 */
	html =
		"<a href=\"/html/?q=x\">next page</a>"
		"<a href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa&amp;rut=1\">icon</a>"
		"<a href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa&amp;rut=1\">Example A</a>"
		"<a href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fother.example%2Fb&amp;rut=2\">Other B</a>";
	n = extractlinks(html, html + strlen(html), "duckduckgo", results, Maxsearchresults);
	ok(n == 2, "extractlinks: nav link filtered, duplicate href deduped");
	if(n == 2){
		okstr(results[0].href, "https://example.com/a", "extractlinks: first result href");
		okstr(results[0].title, "Example A", "extractlinks: dedup keeps longer title");
		okstr(results[1].href, "https://other.example/b", "extractlinks: second result href");
		okstr(results[1].title, "Other B", "extractlinks: second result title");
	}
	freesearchresults(results, n);

	/* no anchors at all -> zero results, not a crash */
	n = extractlinks("<p>no links here</p>",
		"<p>no links here</p>" + strlen("<p>no links here</p>"),
		"duckduckgo", results, Maxsearchresults);
	ok(n == 0, "extractlinks: no anchors yields zero results");
}

/*
 * --- claude.c: web_fetch's URL allowlist and HTML-stripping
 * helpers.  toolwebfetch's permission check happens before any
 * network I/O, so its rejection paths are pure logic and safe
 * to test here; the success path (an actual fetch) is
 * deliberately NOT exercised, per this file's no-network rule
 * (see the header comment) -- urlsearched() is checked directly
 * instead of calling toolwebfetch on a permitted URL.
 */

static void
twebfetch(void)
{
	Conv *c;
	char *s, *res, buf[64];
	int i;

	/* stripblock: removes a tag's body, case-insensitively */
	s = stripblock("before<script>var x=1;</script>after", "script");
	okstr(s, "beforeafter", "stripblock removes script body");
	free(s);

	s = stripblock("a<STYLE type=\"text/css\">.x{color:red}</STYLE>b", "style");
	okstr(s, "ab", "stripblock matches tag name case-insensitively");
	free(s);

	/* a tag name that merely starts with the pattern is not a match */
	s = stripblock("keep<scripted>not a script tag</scripted>keep2", "script");
	okstr(s, "keep<scripted>not a script tag</scripted>keep2",
		"stripblock requires a tag boundary, not a bare prefix match");
	free(s);

	/* no closing tag: drop to the end rather than loop or crash */
	s = stripblock("a<script>never closed", "script");
	okstr(s, "a", "stripblock drops to end when no closing tag is found");
	free(s);

	/* rememberurl / urlsearched: dedup and lookup */
	c = convnew("k", "m", 100, "s", nil);
	rememberurl(c, "https://a.example/1");
	rememberurl(c, "https://b.example/2");
	rememberurl(c, "https://a.example/1");	/* exact repeat */
	ok(c->nsearchurls == 2, "rememberurl dedups exact repeats");
	ok(urlsearched(c, "https://a.example/1"), "urlsearched finds remembered url");
	ok(urlsearched(c, "https://b.example/2"), "urlsearched finds second remembered url");
	ok(!urlsearched(c, "https://c.example/3"), "urlsearched rejects a url never searched");

	/* eviction: oldest entries drop once Maxkepturls is exceeded */
	for(i = 0; i < Maxkepturls + 5; i++){
		snprint(buf, sizeof buf, "https://cap.example/%d", i);
		rememberurl(c, buf);
	}
	ok(c->nsearchurls == Maxkepturls, "rememberurl caps total remembered urls");
	ok(!urlsearched(c, "https://a.example/1"),
		"rememberurl evicts oldest entries once over cap");
	snprint(buf, sizeof buf, "https://cap.example/%d", Maxkepturls + 4);
	ok(urlsearched(c, buf), "rememberurl keeps the most recently added entry");
	convfree(c);

	/* toolwebfetch: every rejection path is checked before any network I/O */
	c = convnew("k", "m", 100, "s", nil);

	res = toolwebfetch(c, nil);
	ok(strncmp(res, "error", 5) == 0, "toolwebfetch: nil url rejected");
	free(res);

	res = toolwebfetch(c, "");
	ok(strncmp(res, "error", 5) == 0, "toolwebfetch: empty url rejected");
	free(res);

	res = toolwebfetch(c, "ftp://example.com/x");
	ok(strncmp(res, "error", 5) == 0, "toolwebfetch: non-http(s) scheme rejected");
	free(res);

	res = toolwebfetch(c, "https://not-searched.example/page");
	ok(strstr(res, "not returned by a previous web_search") != nil,
		"toolwebfetch: url absent from this session's search results is rejected");
	free(res);

	rememberurl(c, "https://searched.example/ok");
	ok(urlsearched(c, "https://searched.example/ok"),
		"toolwebfetch: a remembered url would now pass the permission check");
	ok(fetchurlallowed("https://searched.example/ok",
		"https://searched.example/ok"),
		"webfetch: exact final URL is accepted");
	ok(!fetchurlallowed("https://searched.example/ok",
		"https://redirected.example/elsewhere"),
		"webfetch: redirected final URL is rejected");
	ok(!fetchurlallowed("https://searched.example/ok", nil),
		"webfetch: unverifiable final URL is rejected");

	convclear(c);
	ok(c->nsearchurls == 0 && c->searchurls == nil,
		"convclear drops remembered search URLs");
	ok(!urlsearched(c, "https://searched.example/ok"),
		"cleared conversation cannot fetch an old search result");
	convfree(c);
}

/* --- openai.c: request assembly --- */

static void
topenaibuildreq(void)
{
	Conv *c;
	Json *req, *msgs, *m, *tcs, *tc, *fn, *tools, *t, *fn2, *params;
	Json *sopts, *parsed, *iu;
	char *args;
	int prov;

	/* verify providerlookup recognises "openai" */
	prov = providerlookup("openai");
	ok(prov >= 0, "openai: providerlookup >= 0");
	if(prov < 0)
		return;

	/* basic conv with a system prompt */
	c = convnew("key", "gpt-4o", 2048, "be terse", nil);
	c->prov = prov;

	/* plain user message */
	convappend(c, msgnew(Muser, "hello", nil));
	req = openaibuildreq(c);
	ok(req != nil, "openai: buildreq returns non-nil");
	if(req == nil){
		convfree(c);
		return;
	}

	/* model and output-token cap (modern field name by default) */
	okstr(jstr(req, "model"), "gpt-4o", "openai: model field");
	ok(jget(req, "max_completion_tokens") != nil,
		"openai: max_completion_tokens present");
	ok(jint(req, "max_completion_tokens") == 2048,
		"openai: max_completion_tokens value");
	ok(jget(req, "max_tokens") == nil,
		"openai: legacy max_tokens absent by default");

	/* stream_options with include_usage=true */
	sopts = jget(req, "stream_options");
	ok(sopts != nil, "openai: stream_options present");
	if(sopts != nil){
		/*
		 * include_usage is a JSON bool; jint() only reads
		 * Jint/Jreal (a bool is not a number), so inspect
		 * the node directly.
		 */
		iu = jget(sopts, "include_usage");
		ok(iu != nil, "openai: include_usage present");
		ok(iu != nil && iu->type == Jbool && iu->ival == 1,
			"openai: include_usage true");
	}

	/* compatible servers may reject the optional stream_options extension */
	c->nostreamopts = 1;
	jsonfree(req);
	req = openaibuildreq(c);
	ok(jget(req, "stream_options") == nil,
		"openai: stream_options omitted after compatibility quirk");
	c->nostreamopts = 0;

	/* reasoning_effort absent when thinkmode == Thinkoff */
	ok(c->thinkmode == Thinkoff, "openai: default thinkmode is Thinkoff");
	ok(jget(req, "reasoning_effort") == nil, "openai: reasoning_effort absent when Thinkoff");

	/* system message is messages[0] */
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem >= 2, "openai: messages array has >= 2 entries");
	if(msgs == nil || msgs->nitem < 2){
		jsonfree(req);
		convfree(c);
		return;
	}
	m = jidx(msgs, 0);
	okstr(jstr(m, "role"), "system", "openai: messages[0] role is system");
	okstr(jstr(m, "content"), "be terse", "openai: messages[0] content is sysprompt");

	/* plain user message */
	m = jidx(msgs, 1);
	okstr(jstr(m, "role"), "user", "openai: user message role");
	okstr(jstr(m, "content"), "hello", "openai: user message content");

	jsonfree(req);

	/* blank user text becomes placeholder */
	convclear(c);
	convappend(c, msgnew(Muser, "  ", nil));
	req = openaibuildreq(c);
	ok(req != nil, "openai: buildreq blank user non-nil");
	if(req != nil){
		msgs = jget(req, "messages");
		/* find the user message (skip system if present) */
		m = nil;
		if(msgs != nil){
			int i;
			for(i = 0; i < msgs->nitem; i++){
				Json *mm;
				char *r;
				mm = jidx(msgs, i);
				r = jstr(mm, "role");
				if(r != nil && strcmp(r, "user") == 0){
					m = mm;
					break;
				}
			}
		}
		ok(m != nil, "openai: blank user: found user message");
		if(m != nil)
			okstr(jstr(m, "content"), "(no text)", "openai: blank user placeholder");
		jsonfree(req);
	}

	/* assistant rawjson: one text block + one tool_use block */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "ignored",
		"[{\"type\":\"text\",\"text\":\"Sure.\"},"
		"{\"type\":\"tool_use\",\"id\":\"call_abc\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"/etc/passwd\"}}]"));
	req = openaibuildreq(c);
	ok(req != nil, "openai: assistant rawjson buildreq non-nil");
	if(req != nil){
		msgs = jget(req, "messages");
		/* find the assistant message */
		m = nil;
		if(msgs != nil){
			int i;
			for(i = 0; i < msgs->nitem; i++){
				Json *mm;
				char *r;
				mm = jidx(msgs, i);
				r = jstr(mm, "role");
				if(r != nil && strcmp(r, "assistant") == 0){
					m = mm;
					break;
				}
			}
		}
		ok(m != nil, "openai: assistant message present");
		if(m != nil){
			/* content is the concatenated text */
			okstr(jstr(m, "content"), "Sure.", "openai: assistant content string");
			/* tool_calls array */
			tcs = jget(m, "tool_calls");
			ok(tcs != nil && tcs->nitem == 1, "openai: tool_calls has 1 entry");
			if(tcs != nil && tcs->nitem == 1){
				tc = jidx(tcs, 0);
				okstr(jstr(tc, "type"), "function", "openai: tool_call type=function");
				okstr(jstr(tc, "id"), "call_abc", "openai: tool_call id");
				fn = jget(tc, "function");
				ok(fn != nil, "openai: tool_call has function");
				if(fn != nil){
					okstr(jstr(fn, "name"), "read_file", "openai: function name");
					/* arguments must be a string */
					args = jstr(fn, "arguments");
					ok(args != nil, "openai: arguments is a string");
					if(args != nil){
						/* the string must itself parse to an object
						 * containing the original input field */
						parsed = jsonparse(args);
						ok(parsed != nil, "openai: arguments string parses as JSON");
						if(parsed != nil){
							ok(parsed->type == Jobject, "openai: arguments parses to object");
							okstr(jstr(parsed, "path"), "/etc/passwd",
								"openai: arguments contains path field");
							jsonfree(parsed);
						}
					}
				}
			}
		}
		jsonfree(req);
	}

	/* tool_result user rawjson -> role "tool" messages */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "",
		"[{\"type\":\"tool_use\",\"id\":\"call_xyz\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"/tmp/f\"}}]"));
	convappend(c, msgnew(Muser, "",
		"[{\"type\":\"tool_result\",\"tool_use_id\":\"call_xyz\","
		"\"content\":\"file contents here\"}]"));
	req = openaibuildreq(c);
	ok(req != nil, "openai: tool_result buildreq non-nil");
	if(req != nil){
		int found;
		msgs = jget(req, "messages");
		found = 0;
		if(msgs != nil){
			int i;
			for(i = 0; i < msgs->nitem; i++){
				Json *mm;
				char *r;
				mm = jidx(msgs, i);
				r = jstr(mm, "role");
				if(r != nil && strcmp(r, "tool") == 0){
					found = 1;
					okstr(jstr(mm, "tool_call_id"), "call_xyz",
						"openai: tool message tool_call_id");
					okstr(jstr(mm, "content"), "file contents here",
						"openai: tool message content");
					break;
				}
			}
		}
		ok(found, "openai: tool_result produces role=tool message");
		jsonfree(req);
	}

	/* tools array shape and count */
	convclear(c);
	convappend(c, msgnew(Muser, "hi", nil));
	req = openaibuildreq(c);
	ok(req != nil, "openai: tools check buildreq non-nil");
	if(req != nil){
		tools = jget(req, "tools");
		ok(tools != nil, "openai: tools array present");
		ok(tools != nil && tools->nitem == 9, "openai: tools array has 9 entries");
		if(tools != nil && tools->nitem > 0){
			t = jidx(tools, 0);
			okstr(jstr(t, "type"), "function", "openai: tools[0].type=function");
			fn2 = jget(t, "function");
			ok(fn2 != nil, "openai: tools[0].function present");
			if(fn2 != nil){
				okstr(jstr(fn2, "name"), "create_file",
					"openai: tools[0].function.name=create_file");
				params = jget(fn2, "parameters");
				ok(params != nil, "openai: tools[0].function.parameters present");
				ok(params != nil && params->type == Jobject,
					"openai: tools[0] parameters is object");
			}
		}
		jsonfree(req);
	}

	/* explicit effort maps to OpenAI reasoning_effort */
	convclear(c);
	convappend(c, msgnew(Muser, "hi", nil));
	c->thinkmode = Thinkadaptive;
	c->effort = estrdup("high");
	req = openaibuildreq(c);
	ok(req != nil, "openai: thinkadaptive buildreq non-nil");
	if(req != nil){
		ok(jget(req, "reasoning_effort") != nil,
			"openai: reasoning_effort present in Thinkadaptive");
		okstr(jstr(req, "reasoning_effort"), "high",
			"openai: reasoning_effort value");
		jsonfree(req);
	}

	/* Thinkoff does not discard an explicitly configured effort. */
	c->thinkmode = Thinkoff;
	convclear(c);
	convappend(c, msgnew(Muser, "hi", nil));
	req = openaibuildreq(c);
	ok(req != nil, "openai: thinkoff buildreq non-nil");
	if(req != nil){
		okstr(jstr(req, "reasoning_effort"), "high",
			"openai: explicit effort remains after Thinkoff");
		jsonfree(req);
	}
	free(c->effort);
	c->effort = nil;

	convfree(c);
}

/* --- openai.c: token-field quirk detection --- */

static void
topenaiquirk(void)
{
	Conv *c;
	Json *req;

	c = convnew("key", "gpt-5", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	convappend(c, msgnew(Muser, "hi", nil));

	/* non-matches: no flip, no retry */
	ok(!openaiquirk(c, nil), "quirk: nil error ignored");
	ok(!openaiquirk(c, "API error: overloaded"), "quirk: unrelated error ignored");
	ok(!openaiquirk(c, "API error: max_completion_tokens is too large"),
		"quirk: value complaint ignored");
	ok(c->oldmaxtok == 0, "quirk: flag untouched by non-matches");

	/* Astra/compat server rejecting optional stream_options */
	ok(openaiquirk(c,
		"API error: Unrecognized request argument supplied: stream_options"),
		"quirk: unrecognized stream_options retries without it");
	ok(c->nostreamopts == 1, "quirk: nostreamopts set");
	req = openaibuildreq(c);
	ok(jget(req, "stream_options") == nil,
		"quirk: stream_options absent after rejection");
	jsonfree(req);
	ok(!openaiquirk(c,
		"API error: Unrecognized request argument supplied: stream_options"),
		"quirk: stream_options fallback only retries once");

	/* old compat server rejecting the modern field name */
	ok(openaiquirk(c,
		"API error: Unrecognized request argument supplied: max_completion_tokens"),
		"quirk: unrecognized max_completion_tokens flips");
	ok(c->oldmaxtok == 1, "quirk: oldmaxtok set");
	req = openaibuildreq(c);
	ok(jget(req, "max_tokens") != nil, "quirk: legacy max_tokens after flip");
	ok(jget(req, "max_completion_tokens") == nil,
		"quirk: modern field absent after flip");
	jsonfree(req);

	/* real OpenAI telling us to use max_completion_tokens flips back */
	ok(openaiquirk(c,
		"API error: Unsupported parameter: 'max_tokens' is not supported "
		"with this model. Use 'max_completion_tokens' instead."),
		"quirk: openai unsupported-parameter error flips back");
	ok(c->oldmaxtok == 0, "quirk: oldmaxtok cleared");
	req = openaibuildreq(c);
	ok(jget(req, "max_completion_tokens") != nil,
		"quirk: modern field restored after flip back");
	jsonfree(req);

	convfree(c);
}

/*
 * --- openai.c: reasoning_effort + tools quirk ladder,
 * Thinkadaptive start: effort value -> omit -> explicit "none"
 * -> dead.  The same error wording drives every rung (the
 * server blames reasoning_effort regardless of what we sent).
 */
static void
topenaiquirkreasoning(void)
{
	Conv *c;
	Json *req;
	char *liveerr;

	liveerr =
		"API error: Function tools with reasoning_effort are not "
		"supported for gpt-5.6-sol in /v1/chat/completions. To use "
		"function tools, do not set reasoning_effort.";

	c = convnew("key", "gpt-5.6-sol", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	convappend(c, msgnew(Muser, "hi", nil));
	c->thinkmode = Thinkadaptive;
	c->effort = estrdup("medium");

	/* reasoning_effort present before any quirk fires */
	req = openaibuildreq(c);
	okstr(jstr(req, "reasoning_effort"), "medium",
		"quirk reasoning: effort value present before quirk");
	jsonfree(req);

	/* non-matches: no state change, no retry */
	ok(!openaiquirk(c, nil), "quirk reasoning: nil error ignored");
	ok(!openaiquirk(c, "API error: overloaded"),
		"quirk reasoning: unrelated error ignored");
	ok(c->reasonquirk == Reffort,
		"quirk reasoning: state untouched by non-matches");

	/* rung 1: effort value rejected -> omit the field, retry */
	ok(openaiquirk(c, liveerr),
		"quirk reasoning: first error requests retry");
	ok(c->reasonquirk == Romit, "quirk reasoning: state Romit");
	req = openaibuildreq(c);
	ok(jget(req, "reasoning_effort") == nil,
		"quirk reasoning: field omitted in Romit");
	/* tools must still be present -- that's the whole point */
	ok(jget(req, "tools") != nil, "quirk reasoning: tools still present");
	jsonfree(req);

	/*
	 * rung 2: absence also rejected (server default reasoning is
	 * on) -> send explicit "none", retry.
	 */
	ok(openaiquirk(c, liveerr),
		"quirk reasoning: second error requests retry");
	ok(c->reasonquirk == Rnone, "quirk reasoning: state Rnone");
	req = openaibuildreq(c);
	okstr(jstr(req, "reasoning_effort"), "none",
		"quirk reasoning: explicit none in Rnone");
	ok(jget(req, "tools") != nil,
		"quirk reasoning: tools still present in Rnone");
	jsonfree(req);

	/*
	 * rung 3: even "none" rejected -> no Chat Completions shape
	 * works; move the conversation to the Responses API, which
	 * takes tools and reasoning together.
	 */
	ok(openaiquirk(c, liveerr),
		"quirk reasoning: third error retries on the responses provider");
	ok(c->prov == providerlookup("responses"),
		"quirk reasoning: provider switched to responses");
	ok(c->baseurl == nil, "quirk reasoning: no baseurl invented");
	ok(c->reasonquirk == Reffort && c->respquirks == 0,
		"quirk reasoning: quirk state reset for the new endpoint");

	/*
	 * Same ladder, but on a session whose baseurl cannot be
	 * rewritten: the switch is refused, the ladder dies, and the
	 * error surfaces.
	 */
	convfree(c);
	c = convnew("key", "gpt-5.6-sol", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	c->baseurl = estrdup("http://gateway.example/v1/weird-endpoint");
	c->thinkmode = Thinkadaptive;
	c->effort = estrdup("medium");
	convappend(c, msgnew(Muser, "hi", nil));
	ok(openaiquirk(c, liveerr), "quirk reasoning (odd baseurl): rung 1");
	ok(openaiquirk(c, liveerr), "quirk reasoning (odd baseurl): rung 2");
	ok(!openaiquirk(c, liveerr),
		"quirk reasoning (odd baseurl): third error gives up (no retry)");
	ok(c->reasonquirk == Rdead, "quirk reasoning (odd baseurl): state Rdead");
	ok(c->prov == providerlookup("openai"),
		"quirk reasoning (odd baseurl): provider unchanged");
	okstr(c->baseurl, "http://gateway.example/v1/weird-endpoint",
		"quirk reasoning (odd baseurl): baseurl unchanged");
	req = openaibuildreq(c);
	ok(jget(req, "reasoning_effort") == nil,
		"quirk reasoning: field suppressed in Rdead");
	jsonfree(req);

	/* terminal: further identical errors never re-fire */
	ok(!openaiquirk(c, liveerr),
		"quirk reasoning: Rdead is terminal");
	ok(c->reasonquirk == Rdead, "quirk reasoning: state stays Rdead");

	convfree(c);
}

/*
 * --- openai.c: the quirk ladder from a Thinkoff start (the
 * live /dev/snarf case): a fresh session never sent
 * reasoning_effort at all, yet the server rejected
 * tools+reasoning -- proof that the server applies a default
 * reasoning effort when the field is absent.  Omitting the
 * field is a no-op there, so the quirk must skip straight to
 * sending an explicit "none" (and must NOT resend a
 * byte-identical body, the original wheel-spinning bug).
 */
static void
topenaiquirkreasoningoff(void)
{
	Conv *c;
	Json *req;
	char *liveerr;

	liveerr =
		"API error: Function tools with reasoning_effort are not "
		"supported for gpt-5.6-sol in /v1/chat/completions. To use "
		"function tools, do not set reasoning_effort.";

	c = convnew("key", "gpt-5.6-sol", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	convappend(c, msgnew(Muser, "hi", nil));

	ok(c->thinkmode == Thinkoff, "quirk reasoning off: starts Thinkoff");
	req = openaibuildreq(c);
	ok(jget(req, "reasoning_effort") == nil,
		"quirk reasoning off: field absent before quirk");
	jsonfree(req);

	/*
	 * The field was never sent, so the only useful move is the
	 * explicit "none" override: retry with a request that
	 * actually differs from the one that failed.
	 */
	ok(openaiquirk(c, liveerr),
		"quirk reasoning off: retry requested straight to none");
	ok(c->reasonquirk == Rnone, "quirk reasoning off: state Rnone");
	req = openaibuildreq(c);
	okstr(jstr(req, "reasoning_effort"), "none",
		"quirk reasoning off: explicit none sent");
	ok(jget(req, "tools") != nil,
		"quirk reasoning off: tools still present");
	jsonfree(req);

	/*
	 * gpt-6-astra (live): "none" is not a valid value at all,
	 * and low/medium would repeat the tools+reasoning conflict.
	 * No Chat Completions request can satisfy this model, so
	 * the retry has to be on the Responses API instead.
	 */
	ok(openaiquirk(c,
		"API error: Unsupported value: 'reasoning_effort' does not support "
		"'none' with this model. Supported values are: 'low', 'medium'"),
		"quirk reasoning off: unsupported none retries on responses");
	ok(c->prov == providerlookup("responses"),
		"quirk reasoning off: provider switched to responses");
	req = responsesbuildreq(c);
	ok(jget(req, "reasoning_effort") == nil && jget(req, "reasoning") == nil,
		"quirk reasoning off: responses request carries no reasoning field");
	ok(jget(req, "tools") != nil, "quirk reasoning off: tools still present");
	jsonfree(req);

	convfree(c);
}

/*
 * --- openai.c: the live gpt-6-astra wording names /v1/responses
 * outright.  That is taken up immediately -- one retry, on the
 * right endpoint, with reasoning kept -- instead of burning
 * rungs on "none".  A Chat Completions baseurl override is
 * rewritten to its Responses sibling; any other override
 * refuses the switch and falls back to the ladder.
 */
static void
tswitchresponses(void)
{
	Conv *c;
	char *astraerr;
	int resp;

	astraerr =
		"API error: Function tools with reasoning_effort are not "
		"supported for gpt-6-astra in /v1/chat/completions. To use "
		"function tools, use /v1/responses or set reasoning_effort "
		"to 'none'.";
	resp = providerlookup("responses");
	ok(resp >= 0, "switch: responses provider exists");
	ok(!providerhasmodels(resp), "switch: responses lists no models of its own");
	ok(providerhasmodels(providerlookup("openai")), "switch: openai lists models");

	/* default endpoint: switch on the first complaint */
	c = convnew("key", "gpt-6-astra", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	c->effort = estrdup("medium");
	convappend(c, msgnew(Muser, "hi", nil));
	ok(openaiquirk(c, astraerr), "switch: astra wording requests retry");
	ok(c->prov == resp, "switch: provider is responses");
	okstr(c->effort, "medium", "switch: effort preserved for the new provider");
	ok(!switchresponses(c), "switch: already on responses -> no second switch");
	/* sendonce now consults the responses hook, which ignores this wording */
	ok(!responsesquirk(c, astraerr), "switch: responses quirk ignores the chat error");
	ok(c->respquirks == 0, "switch: responses quirk state untouched");
	convfree(c);

	/* chat/completions baseurl is rewritten to /responses */
	c = convnew("key", "gpt-6-astra", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	c->baseurl = estrdup("https://astra.example/v1/chat/completions");
	convappend(c, msgnew(Muser, "hi", nil));
	ok(openaiquirk(c, astraerr), "switch (baseurl): retry requested");
	ok(c->prov == resp, "switch (baseurl): provider is responses");
	okstr(c->baseurl, "https://astra.example/v1/responses",
		"switch (baseurl): endpoint rewritten");
	convfree(c);

	/* unrecognizable baseurl: no switch, ladder instead */
	c = convnew("key", "gpt-6-astra", 1234, "sys", nil);
	c->prov = providerlookup("openai");
	c->baseurl = estrdup("https://astra.example/api/chat");
	convappend(c, msgnew(Muser, "hi", nil));
	ok(openaiquirk(c, astraerr), "switch (odd baseurl): ladder still retries");
	ok(c->prov == providerlookup("openai"), "switch (odd baseurl): provider unchanged");
	ok(c->reasonquirk == Rnone, "switch (odd baseurl): ladder advanced to Rnone");
	okstr(c->baseurl, "https://astra.example/api/chat",
		"switch (odd baseurl): baseurl untouched");
	convfree(c);

	/* direct switchresponses on a Conv already there is a no-op */
	c = convnew("key", "gpt-6-astra", 1234, "sys", nil);
	c->prov = resp;
	ok(!switchresponses(c), "switch: no-op when already on responses");
	convfree(c);
}

/* --- responses.c: request assembly --- */

static void
tresponsesbuildreq(void)
{
	Conv *c;
	Json *req, *input, *item, *tools, *t, *inc, *reason, *parts, *st;
	int i, nfc, nfco, nreason, nmsg;

	c = convnew("key", "gpt-6-astra", 2048, "be terse", nil);
	c->prov = providerlookup("responses");
	convappend(c, msgnew(Muser, "hello", nil));
	req = responsesbuildreq(c);
	ok(req != nil, "responses: buildreq non-nil");
	if(req == nil){
		convfree(c);
		return;
	}
	okstr(jstr(req, "model"), "gpt-6-astra", "responses: model");
	ok(jint(req, "max_output_tokens") == 2048, "responses: max_output_tokens");
	ok(jget(req, "max_completion_tokens") == nil && jget(req, "max_tokens") == nil,
		"responses: no chat-completions token field");
	okstr(jstr(req, "instructions"), "be terse", "responses: system prompt as instructions");
	st = jget(req, "store");
	ok(st != nil && st->type == Jbool && st->ival == 0, "responses: store false");
	inc = jget(req, "include");
	ok(inc != nil && inc->type == Jarray && inc->nitem == 1
		&& strcmp(jidx(inc, 0)->str, "reasoning.encrypted_content") == 0,
		"responses: include encrypted reasoning");
	ok(jget(req, "reasoning") == nil, "responses: no reasoning object without effort");
	ok(jget(req, "reasoning_effort") == nil, "responses: no chat-style reasoning_effort");
	ok(jget(req, "stream_options") == nil, "responses: no stream_options");

	input = jget(req, "input");
	ok(input != nil && input->type == Jarray && input->nitem == 1,
		"responses: one input item for one user message");
	item = jidx(input, 0);
	okstr(jstr(item, "role"), "user", "responses: user item role");
	okstr(jstr(item, "content"), "hello", "responses: user item content");

	tools = jget(req, "tools");
	ok(tools != nil && tools->nitem == 9, "responses: 9 tools");
	if(tools != nil && tools->nitem > 0){
		t = jidx(tools, 0);
		okstr(jstr(t, "type"), "function", "responses: tool type");
		okstr(jstr(t, "name"), "create_file", "responses: tool name is top-level");
		ok(jget(t, "function") == nil, "responses: tool not nested under function");
		ok(jget(t, "parameters") != nil, "responses: tool parameters present");
	}
	jsonfree(req);

	/* effort -> reasoning {effort, summary} */
	c->effort = estrdup("medium");
	req = responsesbuildreq(c);
	reason = jget(req, "reasoning");
	ok(reason != nil, "responses: reasoning object with effort");
	okstr(jstr(reason, "effort"), "medium", "responses: reasoning.effort");
	okstr(jstr(reason, "summary"), "auto", "responses: reasoning.summary auto");
	jsonfree(req);
	c->respquirks |= Rqnosummary;
	req = responsesbuildreq(c);
	ok(jget(jget(req, "reasoning"), "summary") == nil,
		"responses: summary omitted after quirk");
	jsonfree(req);
	c->respquirks = 0;
	free(c->effort);
	c->effort = estrdup("none");
	req = responsesbuildreq(c);
	okstr(jstr(jget(req, "reasoning"), "effort"), "none", "responses: effort none passed through");
	ok(jget(jget(req, "reasoning"), "summary") == nil,
		"responses: no summary with effort none");
	jsonfree(req);
	free(c->effort);
	c->effort = nil;

	/*
	 * A full tool round as stored by responsesreadstream:
	 * reasoning item, text with item_id, function_call with
	 * item_id, then the tool result and a follow-up prompt.
	 */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "Looking.",
		"[{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],"
		"\"encrypted_content\":\"opaque\"},"
		"{\"type\":\"text\",\"text\":\"Looking.\",\"item_id\":\"msg_1\"},"
		"{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"/tmp/f\"},\"item_id\":\"fc_1\"}]"));
	convappend(c, msgnew(Muser, "",
		"[{\"type\":\"tool_result\",\"tool_use_id\":\"call_1\","
		"\"content\":\"contents\"}]"));
	convappend(c, msgnew(Muser, "thanks", nil));
	req = responsesbuildreq(c);
	input = jget(req, "input");
	ok(input != nil && input->nitem == 6, "responses: replay item count");
	if(input != nil && input->nitem == 6){
		okstr(jstr(jidx(input, 0), "role"), "user", "responses: replay[0] user");
		item = jidx(input, 1);
		okstr(jstr(item, "type"), "reasoning", "responses: replay[1] reasoning");
		okstr(jstr(item, "id"), "rs_1", "responses: reasoning id kept");
		okstr(jstr(item, "encrypted_content"), "opaque", "responses: encrypted content kept");
		item = jidx(input, 2);
		okstr(jstr(item, "type"), "message", "responses: replay[2] message");
		okstr(jstr(item, "role"), "assistant", "responses: assistant message role");
		okstr(jstr(item, "id"), "msg_1", "responses: message item id replayed");
		parts = jget(item, "content");
		ok(parts != nil && parts->nitem == 1, "responses: one content part");
		okstr(jstr(jidx(parts, 0), "type"), "output_text", "responses: output_text part");
		okstr(jstr(jidx(parts, 0), "text"), "Looking.", "responses: part text");
		item = jidx(input, 3);
		okstr(jstr(item, "type"), "function_call", "responses: replay[3] function_call");
		okstr(jstr(item, "id"), "fc_1", "responses: function_call item id");
		okstr(jstr(item, "call_id"), "call_1", "responses: function_call call_id");
		okstr(jstr(item, "name"), "read_file", "responses: function_call name");
		{
			Json *args;
			args = jsonparse(jstr(item, "arguments") ? jstr(item, "arguments") : "");
			ok(args != nil && args->type == Jobject, "responses: arguments is a JSON string");
			okstr(jstr(args, "path"), "/tmp/f", "responses: arguments content");
			jsonfree(args);
		}
		item = jidx(input, 4);
		okstr(jstr(item, "type"), "function_call_output", "responses: replay[4] output");
		okstr(jstr(item, "call_id"), "call_1", "responses: output call_id");
		okstr(jstr(item, "output"), "contents", "responses: output text");
		item = jidx(input, 5);
		okstr(jstr(item, "role"), "user", "responses: replay[5] follow-up user");
		okstr(jstr(item, "content"), "thanks", "responses: follow-up text");
	}
	jsonfree(req);

	/* Rqnoreasonitems drops reasoning items and every item id */
	c->respquirks |= Rqnoreasonitems;
	req = responsesbuildreq(c);
	input = jget(req, "input");
	nfc = nfco = nreason = nmsg = 0;
	for(i = 0; input != nil && i < input->nitem; i++){
		char *ty;
		item = jidx(input, i);
		ty = jstr(item, "type");
		if(ty == nil) continue;
		if(strcmp(ty, "reasoning") == 0) nreason++;
		if(strcmp(ty, "function_call") == 0){ nfc++; ok(jget(item, "id") == nil, "responses: fc id dropped"); }
		if(strcmp(ty, "function_call_output") == 0) nfco++;
		if(strcmp(ty, "message") == 0){ nmsg++; ok(jget(item, "id") == nil, "responses: msg id dropped"); }
	}
	ok(nreason == 0 && nfc == 1 && nfco == 1 && nmsg == 1,
		"responses: no reasoning items after quirk, rest intact");
	ok(input != nil && input->nitem == 5, "responses: item count after quirk");
	jsonfree(req);
	c->respquirks = 0;

	/* Rqnoinclude drops store/include */
	c->respquirks |= Rqnoinclude;
	req = responsesbuildreq(c);
	ok(jget(req, "store") == nil && jget(req, "include") == nil,
		"responses: store/include omitted after quirk");
	jsonfree(req);
	c->respquirks = 0;

	/* anthropic-only blocks are skipped, not sent */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "a",
		"[{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"sig\"},"
		"{\"type\":\"text\",\"text\":\"a\"}]"));
	req = responsesbuildreq(c);
	input = jget(req, "input");
	ok(input != nil && input->nitem == 2, "responses: thinking block skipped");
	jsonfree(req);

	convfree(c);
}

/* --- responses.c: quirk hook --- */

static void
tresponsesquirk(void)
{
	Conv *c;

	c = convnew("key", "gpt-6-astra", 1234, "sys", nil);
	c->prov = providerlookup("responses");

	ok(!responsesquirk(c, nil), "responses quirk: nil ignored");
	ok(!responsesquirk(c, "API error: overloaded"), "responses quirk: unrelated ignored");
	ok(!responsesquirk(c,
		"API error: Unsupported parameter: 'reasoning.effort' is not supported with this model"),
		"responses quirk: effort rejection surfaces (user setting)");
	ok(c->respquirks == 0, "responses quirk: state untouched by non-matches");

	ok(responsesquirk(c, "API error: Unknown parameter: 'include'."),
		"responses quirk: unknown include retries");
	ok(c->respquirks & Rqnoinclude, "responses quirk: Rqnoinclude set");
	ok(!responsesquirk(c, "API error: Unknown parameter: 'include'."),
		"responses quirk: include only retried once");

	ok(responsesquirk(c,
		"API error: Unsupported parameter: 'reasoning.summary' is not supported with this model."),
		"responses quirk: summary rejection retries");
	ok(c->respquirks & Rqnosummary, "responses quirk: Rqnosummary set");

	ok(responsesquirk(c,
		"API error: Item 'rs_abc' of type 'reasoning' was provided without its required following item."),
		"responses quirk: unpaired reasoning item retries");
	ok(c->respquirks & Rqnoreasonitems, "responses quirk: Rqnoreasonitems set");
	ok(!responsesquirk(c,
		"API error: Item 'fc_abc' of type 'function_call' was provided without its required 'reasoning' item: 'rs_abc'."),
		"responses quirk: reasoning-item errors are terminal once stripped");

	convfree(c);
}

/* --- responses.c: stream parsing --- */

static void
tresponsesstream(void)
{
	char *path, *sse;
	Biobuf *bp;
	Usage u;
	Reply *r;
	Json *raw, *b;
	ToolCall *tc;

	/*
	 * A reasoning model's tool round: reasoning item (with an
	 * encrypted body and a streamed summary), a text message,
	 * a function call with its arguments streamed in pieces,
	 * then response.completed carrying the full output array
	 * and usage.
	 */
	sse =
		"event: response.created\n"
		"data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\",\"status\":\"in_progress\"}}\n"
		"\n"
		"data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[]}}\n"
		"\n"
		"data: {\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_1\",\"delta\":\"Need the file.\"}\n"
		"\n"
		"data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"Need the file.\"}],\"encrypted_content\":\"opaque\"}}\n"
		"\n"
		"data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n"
		"\n"
		"data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"delta\":\"Let me \"}\n"
		"\n"
		"data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"delta\":\"look.\"}\n"
		"\n"
		"data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_1\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Let me look.\",\"annotations\":[]}]}}\n"
		"\n"
		"data: {\"type\":\"response.output_item.added\",\"output_index\":2,\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"\"}}\n"
		"\n"
		"data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"{\\\"pa\"}\n"
		"\n"
		"data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"th\\\": \\\"/tmp/x\\\"}\"}\n"
		"\n"
		"data: {\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\": \\\"/tmp/x\\\"}\"}}\n"
		"\n"
		"data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"status\":\"completed\","
		"\"output\":["
		"{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"Need the file.\"}],\"encrypted_content\":\"opaque\"},"
		"{\"id\":\"msg_1\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Let me look.\",\"annotations\":[]}]},"
		"{\"id\":\"fc_1\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\": \\\"/tmp/x\\\"}\"}"
		"],"
		"\"usage\":{\"input_tokens\":10,\"input_tokens_details\":{\"cached_tokens\":3},\"output_tokens\":5,\"output_tokens_details\":{\"reasoning_tokens\":2},\"total_tokens\":15}}}\n"
		"\n";

	path = writetmpsse(sse);
	ok(path != nil, "responses stream: temp file");
	if(path == nil)
		return;
	bp = Bopen(path, OREAD);
	memset(&u, 0, sizeof u);
	r = responsesreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);

	ok(r != nil, "responses stream: parsed");
	if(r == nil)
		return;
	okstr(r->text, "Let me look.", "responses stream: text");
	okstr(u.stop_reason, "tool_use", "responses stream: stop_reason tool_use");
	ok(r->stopped == 0, "responses stream: stopped==0 with a function call");
	ok(u.input_tokens == 10 && u.output_tokens == 5 && u.cache_read_input_tokens == 3,
		"responses stream: usage mapped");

	tc = r->tools;
	ok(tc != nil, "responses stream: tool call present");
	if(tc != nil){
		okstr(tc->id, "call_1", "responses stream: tool call id is call_id");
		okstr(tc->name, "read_file", "responses stream: tool name");
		okstr(tc->args[0], "/tmp/x", "responses stream: tool arg parsed");
		ok(tc->next == nil, "responses stream: exactly one tool call");
	}

	raw = jsonparse(r->rawjson);
	ok(raw != nil && raw->type == Jarray && raw->nitem == 3, "responses stream: three neutral blocks");
	if(raw != nil && raw->nitem == 3){
		b = jidx(raw, 0);
		okstr(jstr(b, "type"), "reasoning", "responses stream: block 0 reasoning");
		okstr(jstr(b, "encrypted_content"), "opaque", "responses stream: reasoning kept verbatim");
		b = jidx(raw, 1);
		okstr(jstr(b, "type"), "text", "responses stream: block 1 text");
		okstr(jstr(b, "text"), "Let me look.", "responses stream: text block text");
		okstr(jstr(b, "item_id"), "msg_1", "responses stream: text block item_id");
		b = jidx(raw, 2);
		okstr(jstr(b, "type"), "tool_use", "responses stream: block 2 tool_use");
		okstr(jstr(b, "id"), "call_1", "responses stream: tool_use id");
		okstr(jstr(b, "item_id"), "fc_1", "responses stream: tool_use item_id");
		okstr(jstr(jget(b, "input"), "path"), "/tmp/x", "responses stream: tool_use input");
	}
	jsonfree(raw);
	replyfree(r);
	free(u.stop_reason);
	u.stop_reason = nil;

	/* incomplete on max_output_tokens -> max_tokens; text only */
	sse =
		"data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Partial\"}]}}\n"
		"data: {\"type\":\"response.incomplete\",\"response\":{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},\"output\":[],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n";
	path = writetmpsse(sse);
	bp = Bopen(path, OREAD);
	memset(&u, 0, sizeof u);
	r = responsesreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);
	ok(r != nil, "responses stream: incomplete parsed");
	if(r != nil){
		okstr(r->text, "Partial", "responses stream: items kept when final output is empty");
		okstr(u.stop_reason, "max_tokens", "responses stream: max_output_tokens -> max_tokens");
		ok(r->stopped == 1, "responses stream: stopped on max_tokens");
		ok(r->tools == nil, "responses stream: no tools");
		replyfree(r);
	}
	free(u.stop_reason);
	u.stop_reason = nil;

	/* response.failed -> error */
	sse = "data: {\"type\":\"response.failed\",\"response\":{\"status\":\"failed\",\"error\":{\"code\":\"server_error\",\"message\":\"boom\"}}}\n";
	path = writetmpsse(sse);
	bp = Bopen(path, OREAD);
	memset(&u, 0, sizeof u);
	r = responsesreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);
	ok(r == nil, "responses stream: failed response returns nil");
	{
		char eb[ERRMAX];
		rerrstr(eb, sizeof eb);
		ok(strstr(eb, "boom") != nil, "responses stream: failure message in errstr");
	}

	/* error event -> error */
	sse = "data: {\"type\":\"error\",\"code\":\"rate_limit\",\"message\":\"slow down\"}\n";
	path = writetmpsse(sse);
	bp = Bopen(path, OREAD);
	r = responsesreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);
	ok(r == nil, "responses stream: error event returns nil");

	/* truncated stream (no terminal event) -> error */
	sse = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n";
	path = writetmpsse(sse);
	bp = Bopen(path, OREAD);
	r = responsesreadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);
	ok(r == nil, "responses stream: truncated stream returns nil");
}

/*
 * --- claude.c: the Anthropic builder strips what only the
 * Responses provider understands, so a conversation begun on
 * responses can continue on anthropic.
 */
static void
tstripforeign(void)
{
	Conv *c;
	Json *req, *msgs, *content, *b;

	c = convnew("key", "claude-opus-4-8", 1000, "sys", nil);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "Looking.",
		"[{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],\"encrypted_content\":\"opaque\"},"
		"{\"type\":\"text\",\"text\":\"Looking.\",\"item_id\":\"msg_1\"},"
		"{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"/tmp/f\"},\"item_id\":\"fc_1\"}]"));
	convappend(c, msgnew(Muser, "",
		"[{\"type\":\"tool_result\",\"tool_use_id\":\"call_1\",\"content\":\"ok\"}]"));
	req = anthropicbuildreq(c);
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 3, "stripforeign: message count unchanged");
	content = jget(jidx(msgs, 1), "content");
	ok(content != nil && content->nitem == 2, "stripforeign: reasoning block dropped");
	if(content != nil && content->nitem == 2){
		b = jidx(content, 0);
		okstr(jstr(b, "type"), "text", "stripforeign: text kept");
		ok(jget(b, "item_id") == nil, "stripforeign: text item_id removed");
		b = jidx(content, 1);
		okstr(jstr(b, "type"), "tool_use", "stripforeign: tool_use kept");
		ok(jget(b, "item_id") == nil, "stripforeign: tool_use item_id removed");
		okstr(jstr(b, "id"), "call_1", "stripforeign: tool_use id intact");
	}
	jsonfree(req);

	/* reasoning-only turn (corrupt snapshot) leaves a placeholder */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "",
		"[{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[]}]"));
	convappend(c, msgnew(Muser, "more", nil));
	req = anthropicbuildreq(c);
	content = jget(jidx(jget(req, "messages"), 1), "content");
	ok(content != nil && content->nitem == 1, "stripforeign: placeholder for emptied turn");
	okstr(jstr(jidx(content, 0), "type"), "text", "stripforeign: placeholder is text");
	jsonfree(req);
	convfree(c);
}

/* --- json.c: key deletion and deep copy --- */

static void
tjsondel(void)
{
	Json *o, *cp;
	char *s;

	o = jsonparse("{\"a\":1,\"b\":[1,2],\"c\":\"x\"}");
	jdel(o, "b");
	s = jsonstr(o);
	okstr(s, "{\"a\":1,\"c\":\"x\"}", "jdel removes middle key, keeps order");
	free(s);
	jdel(o, "nosuch");
	ok(o->nitem == 2, "jdel missing key is a no-op");
	jdel(o, "a");
	jdel(o, "c");
	ok(o->nitem == 0, "jdel empties object");
	jdel(nil, "a");
	jsonfree(o);

	o = jsonparse("{\"a\":[1,{\"b\":\"c\"}]}");
	cp = jcopy(o);
	jdel(o, "a");
	s = jsonstr(cp);
	okstr(s, "{\"a\":[1,{\"b\":\"c\"}]}", "jcopy is independent of the original");
	free(s);
	ok(jcopy(nil) == nil, "jcopy nil");
	jsonfree(o);
	jsonfree(cp);
}

/* write a string to a file for use as a canned SSE transcript */
static char*
writetmpsse(char *content)
{
	char *path;
	int fd;

	path = esmprint("/tmp/claudetest.sse.%d", getpid());
	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		free(path);
		return nil;
	}
	write(fd, content, strlen(content));
	close(fd);
	return path;
}

/* --- openai.c: stream parsing with tool calls --- */

static void
topenaistream(void)
{
	char *path, *sse;
	Biobuf *bp;
	Usage u;
	Reply *r;
	Json *rawj, *blocks, *blk;
	ToolCall *tc;
	int ntc;

	sse =
		"data: {\"choices\":[{\"delta\":{\"content\":\"Let me \"},\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"look.\"},\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
		"\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"pa\"}}]},"
		"\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
		"\"function\":{\"arguments\":\"th\\\": \\\"/tmp/x\\\"}\"}}]},"
		"\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n"
		"\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,"
		"\"prompt_tokens_details\":{\"cached_tokens\":3}}}\n"
		"\n"
		"data: [DONE]\n";

	path = writetmpsse(sse);
	ok(path != nil, "openai stream: created temp file");
	if(path == nil)
		return;

	bp = Bopen(path, OREAD);
	ok(bp != nil, "openai stream: Bopen temp file");
	if(bp == nil){
		remove(path);
		free(path);
		return;
	}

	memset(&u, 0, sizeof u);
	r = openaireadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);

	ok(r != nil, "openai stream: readstream returns non-nil");
	if(r == nil)
		return;

	/* text accumulation */
	okstr(r->text, "Let me look.", "openai stream: text accumulated");

	/* stop reason normalization: tool_calls -> tool_use */
	okstr(u.stop_reason, "tool_use", "openai stream: stop_reason tool_use");

	/* stopped == 0 when finish_reason was tool_calls */
	ok(r->stopped == 0, "openai stream: stopped==0 for tool_calls");

	/* usage mapping */
	ok(u.input_tokens == 10, "openai stream: input_tokens");
	ok(u.output_tokens == 5, "openai stream: output_tokens");
	ok(u.cache_read_input_tokens == 3, "openai stream: cache_read_input_tokens");

	/* tool call list */
	tc = r->tools;
	ok(tc != nil, "openai stream: tools list non-nil");
	if(tc != nil){
		okstr(tc->id, "call_1", "openai stream: tc->id");
		okstr(tc->name, "read_file", "openai stream: tc->name");
		okstr(tc->args[0], "/tmp/x", "openai stream: tc->args[0]");
		ok(tc->next == nil, "openai stream: exactly one tool call");
	}

	/* rawjson: parse and inspect blocks */
	ok(r->rawjson != nil, "openai stream: rawjson non-nil");
	if(r->rawjson != nil){
		rawj = jsonparse(r->rawjson);
		ok(rawj != nil, "openai stream: rawjson parses");
		if(rawj != nil){
			ok(rawj->type == Jarray, "openai stream: rawjson is array");
			/* find text block */
			blocks = rawj;
			ntc = 0;
			{
				int i;
				int gottxt;
				int gottc;
				gottxt = 0;
				gottc = 0;
				for(i = 0; i < blocks->nitem; i++){
					char *btype;
					blk = jidx(blocks, i);
					btype = jstr(blk, "type");
					if(btype != nil && strcmp(btype, "text") == 0){
						gottxt = 1;
						okstr(jstr(blk, "text"), "Let me look.",
							"openai stream: rawjson text block text");
					}
					if(btype != nil && strcmp(btype, "tool_use") == 0){
						gottc = 1;
						ntc++;
						okstr(jstr(blk, "id"), "call_1",
							"openai stream: rawjson tool_use id");
						okstr(jstr(blk, "name"), "read_file",
							"openai stream: rawjson tool_use name");
						ok(jget(blk, "input") != nil,
							"openai stream: rawjson tool_use input present");
					}
				}
				ok(gottxt, "openai stream: rawjson has text block");
				ok(gottc, "openai stream: rawjson has tool_use block");
				ok(ntc == 1, "openai stream: rawjson has exactly one tool_use");
			}
			jsonfree(rawj);
		}
	}

	replyfree(r);
}

/* --- openai.c: text-only stream and truncated stream --- */

static void
topenaistream2(void)
{
	char *path, *path2, *sse, *sse2;
	Biobuf *bp;
	Usage u;
	Reply *r;
	Json *rawj, *blocks, *blk;
	int gottxt, i;

	/* text-only transcript ending with finish_reason "stop" */
	sse =
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"},\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":null}]}\n"
		"\n"
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
		"\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":2,"
		"\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n"
		"\n"
		"data: [DONE]\n";

	path = writetmpsse(sse);
	ok(path != nil, "openai stream2: created text-only temp file");
	if(path == nil)
		goto trunc;

	bp = Bopen(path, OREAD);
	ok(bp != nil, "openai stream2: Bopen text-only file");
	if(bp == nil){
		remove(path);
		free(path);
		goto trunc;
	}

	memset(&u, 0, sizeof u);
	r = openaireadstream(nil, bp, &u, nil, nil);
	Bterm(bp);
	remove(path);
	free(path);

	ok(r != nil, "openai stream2: text-only returns non-nil");
	if(r != nil){
		/* text */
		okstr(r->text, "Hello world", "openai stream2: text-only text");

		/* stop_reason: "stop" -> "end_turn" */
		okstr(u.stop_reason, "end_turn", "openai stream2: stop_reason end_turn");

		/* stopped == 1 for stop finish_reason */
		ok(r->stopped == 1, "openai stream2: stopped==1 for stop");

		/* no tool calls */
		ok(r->tools == nil, "openai stream2: no tool calls");

		/* rawjson has exactly one text block */
		ok(r->rawjson != nil, "openai stream2: rawjson non-nil");
		if(r->rawjson != nil){
			rawj = jsonparse(r->rawjson);
			ok(rawj != nil, "openai stream2: rawjson parses");
			if(rawj != nil){
				ok(rawj->type == Jarray, "openai stream2: rawjson is array");
				blocks = rawj;
				gottxt = 0;
				ok(blocks->nitem == 1, "openai stream2: rawjson has exactly 1 block");
				for(i = 0; i < blocks->nitem; i++){
					char *btype;
					blk = jidx(blocks, i);
					btype = jstr(blk, "type");
					if(btype != nil && strcmp(btype, "text") == 0)
						gottxt = 1;
				}
				ok(gottxt, "openai stream2: rawjson block is text type");
				jsonfree(rawj);
			}
		}
		replyfree(r);
	}

trunc:
	/* truncated transcript (no [DONE]) -> nil return */
	sse2 =
		"data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"},\"finish_reason\":null}]}\n"
		"\n";

	path2 = esmprint("/tmp/claudetest.sse2.%d", getpid());
	{
		int fd;
		fd = create(path2, OWRITE, 0666);
		ok(fd >= 0, "openai stream2: created truncated temp file");
		if(fd >= 0){
			write(fd, sse2, strlen(sse2));
			close(fd);
		} else {
			free(path2);
			return;
		}
	}

	bp = Bopen(path2, OREAD);
	ok(bp != nil, "openai stream2: Bopen truncated file");
	if(bp != nil){
		Usage u2;
		Reply *r2;
		memset(&u2, 0, sizeof u2);
		r2 = openaireadstream(nil, bp, &u2, nil, nil);
		Bterm(bp);
		ok(r2 == nil, "openai stream2: truncated stream returns nil");
		if(r2 != nil)
			replyfree(r2);
	}
	remove(path2);
	free(path2);
}

void
threadmain(int argc, char **argv)
{
	USED(argc);
	USED(argv);

	tjsonparse();
	tjsonstring();
	tjsonbuild();
	tblankstr();
	tstrip();
	trepairuse();
	trepairresults();
	tbuildreq();
	tcompact();
	tpathhash();
	tsbuf();
	terrs();
	treadlimit();
	treplace();
	tmkparents();
	ttoolman();
	twebsearch();
	twebfetch();
	tadvisorstream();
	topenaibuildreq();
	topenaiquirk();
	topenaiquirkreasoning();
	topenaiquirkreasoningoff();
	topenaistream();
	topenaistream2();
	tswitchresponses();
	tresponsesbuildreq();
	tresponsesquirk();
	tresponsesstream();
	tstripforeign();
	tjsondel();

	if(nfail > 0){
		fprint(2, "%d of %d tests FAILED\n", nfail, nrun);
		threadexitsall("fail");
	}
	print("all %d tests passed\n", nrun);
	threadexitsall(nil);
}
