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
		ok(tools != nil && tools->nitem == 7, "openai: tools array has 7 entries");
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

	/* reasoning_effort present only in Thinkadaptive mode */
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

	/* back to Thinkoff -> reasoning_effort absent */
	c->thinkmode = Thinkoff;
	convclear(c);
	convappend(c, msgnew(Muser, "hi", nil));
	req = openaibuildreq(c);
	ok(req != nil, "openai: thinkoff buildreq non-nil");
	if(req != nil){
		ok(jget(req, "reasoning_effort") == nil,
			"openai: reasoning_effort absent after Thinkoff");
		jsonfree(req);
	}

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

	/* rung 3: even "none" rejected -> dead, no retry, field gone */
	ok(!openaiquirk(c, liveerr),
		"quirk reasoning: third error gives up (no retry)");
	ok(c->reasonquirk == Rdead, "quirk reasoning: state Rdead");
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

	/* if "none" is rejected too, give up rather than loop */
	ok(!openaiquirk(c, liveerr),
		"quirk reasoning off: second error gives up");
	ok(c->reasonquirk == Rdead, "quirk reasoning off: state Rdead");
	req = openaibuildreq(c);
	ok(jget(req, "reasoning_effort") == nil,
		"quirk reasoning off: field suppressed in Rdead");
	jsonfree(req);

	convfree(c);
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
	topenaibuildreq();
	topenaiquirk();
	topenaiquirkreasoning();
	topenaiquirkreasoningoff();
	topenaistream();
	topenaistream2();

	if(nfail > 0){
		fprint(2, "%d of %d tests FAILED\n", nfail, nrun);
		threadexitsall("fail");
	}
	print("all %d tests passed\n", nrun);
	threadexitsall(nil);
}
