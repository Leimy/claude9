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
	req = buildreq(c);
	okstr(jstr(req, "model"), "test-model", "buildreq model");
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 1, "one message");
	content = jget(jidx(msgs, 0), "content");
	ok(content != nil && content->nitem == 1, "one content block");
	okstr(jstr(jidx(content, 0), "text"), "hello", "prompt text");
	jsonfree(req);

	/* consecutive same-role messages merge into one */
	convappend(c, msgnew(Muser, "again", nil));
	req = buildreq(c);
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 1, "same-role messages merge");
	content = jget(jidx(msgs, 0), "content");
	ok(content != nil && content->nitem == 2, "merged content blocks");
	jsonfree(req);

	/* blank text becomes a placeholder, never an empty block */
	convclear(c);
	convappend(c, msgnew(Muser, "  \n", nil));
	req = buildreq(c);
	content = jget(jidx(jget(req, "messages"), 0), "content");
	okstr(jstr(jidx(content, 0), "text"), "(no text)", "blank placeholder");
	jsonfree(req);

	/* blank text blocks inside a replayed rawjson snapshot are stripped */
	convclear(c);
	convappend(c, msgnew(Muser, "q", nil));
	convappend(c, msgnew(Massistant, "ans",
		"[{\"type\":\"text\",\"text\":\" \"},"
		"{\"type\":\"text\",\"text\":\"ans\"}]"));
	req = buildreq(c);
	msgs = jget(req, "messages");
	ok(msgs != nil && msgs->nitem == 2, "user + assistant messages");
	content = jget(jidx(msgs, 1), "content");
	ok(content != nil && content->nitem == 1, "blank block stripped from rawjson");
	okstr(jstr(jidx(content, 0), "text"), "ans", "real block survives");
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
	ok(!toollimiterr("some other error"), "toollimiterr non-match");
	ok(!toollimiterr(nil), "toollimiterr nil");
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

	res = toolreplace(path, "world", "there");
	ok(strncmp(res, "error", 5) != 0, "replace succeeds");
	free(res);
	res = toolread(path);
	okstr(res, "hello there hello", "replace result");
	free(res);

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
	treplace();
	tmkparents();
	ttoolman();

	if(nfail > 0){
		fprint(2, "%d of %d tests FAILED\n", nfail, nrun);
		threadexitsall("fail");
	}
	print("all %d tests passed\n", nrun);
	threadexitsall(nil);
}
