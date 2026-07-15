/*
 * OpenAI Chat Completions provider for claude9.
 *
 * Implements the openai entry of the provider vtable (see
 * claudeimpl.h and PROVIDERS.md): request assembly from the
 * neutral conversation form, and streamed response parsing
 * back into a Reply carrying neutral rawjson.
 *
 * This wire format is the de-facto standard implemented by
 * many backends (OpenAI itself, llama.cpp, vllm, OpenRouter);
 * point Conv.baseurl at a compatible server to use one.
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"
#include "claudeimpl.h"

/* OpenAI auth: bearer token. */
int
openaiheaders(int fd, char *apikey)
{
	if(fprint(fd, "headers Authorization: Bearer %s\r\n", apikey) < 0)
		return -1;
	return 0;
}

/*
 * Provider quirk hook (see the Provider.quirk slot in
 * claudeimpl.h): inspect a failed request's error string for a
 * fixable request-shape complaint; adjust the conversation and
 * return 1 to have sendonce rebuild and resend (up to
 * Maxquirks retries per round; see sendonce in claude.c).
 *
 * Two known cases so far: the output-token field name, and
 * reasoning_effort colliding with function tools (see below).
 *
 * Case 1: the output-token field name.  We send
 * max_completion_tokens by default (see openaibuildreq); an
 * old openai-compatible server that only knows the original
 * max_tokens spelling rejects it with an unknown/unsupported-
 * parameter error, and real OpenAI names the field explicitly
 * when told to use it ("Unsupported parameter: 'max_tokens' is
 * not supported with this model. Use 'max_completion_tokens'
 * instead.").  Both directions therefore mention
 * "max_completion_tokens", so one rule covers them: an error
 * naming that field with unsupported/unknown/unrecognized
 * phrasing toggles Conv.oldmaxtok.  The quirk is per-server,
 * not per-model; sendonce caps quirk retries per round at
 * Maxquirks, so even a toggling misdiagnosis costs a few
 * extra requests at worst and the real error still surfaces.
 *
 * Case 2: reasoning_effort + tools.  See the comment further
 * down, at the check itself.
 */
int
openaiquirk(Conv *c, char *err)
{
	if(err == nil)
		return 0;
	if(strstr(err, "max_completion_tokens") != nil
	&& (strstr(err, "not supported") != nil	/* "is not supported with this model" */
	 || strstr(err, "nsupported") != nil	/* Unsupported/unsupported parameter */
	 || strstr(err, "nrecognized") != nil	/* Unrecognized request argument */
	 || strstr(err, "nknown") != nil	/* unknown field/parameter */
	 || strstr(err, "not permitted") != nil)){	/* pydantic "extra inputs are not permitted" */
		c->oldmaxtok = !c->oldmaxtok;
		return 1;
	}
	/*
	 * Some models (observed: a gpt-5.x variant) reject
	 * function tools when reasoning is in effect, e.g.
	 * "Function tools with reasoning_effort are not supported
	 * for <model> in /v1/chat/completions.  To use function
	 * tools, do not set reasoning_effort" (exact wording may
	 * vary by server).  Tools are load-bearing for this
	 * program, so the fix is always on the reasoning side.
	 *
	 * The catch (learned live, the hard way): "not set" is
	 * judged server-side, not by what we sent.  A fresh
	 * Thinkoff session that never emitted the field still got
	 * this rejection, which means the server applies a DEFAULT
	 * reasoning effort for the model when the field is absent.
	 * Omitting the field therefore cannot fix that case; the
	 * only way to turn reasoning off is to send an explicit
	 * reasoning_effort "none" overriding the default.
	 *
	 * So instead of a single omit-latch, walk a monotonic
	 * ladder (Conv.reasonquirk, see claude.h), one rung per
	 * complaint:
	 *
	 *   Reffort  and we sent an effort value -> Romit:
	 *            drop the field, retry (maybe the server has
	 *            no default and just hates the value+tools).
	 *   Reffort  and the field was absent (Thinkoff: the live
	 *            case) -> Rnone directly: the server default
	 *            must be on; send "none", retry.
	 *   Romit    (absence still rejected) -> Rnone: send
	 *            "none", retry.
	 *   Rnone    (even "none" rejected) -> Rdead: suppress the
	 *            field and give up; this model+server really
	 *            cannot do tools, and the error should surface.
	 *   Rdead    terminal; never retry on this error again.
	 *
	 * Monotonic means it terminates: at most three retries over
	 * the life of a Conv, no ping-pong.
	 */
	if(strstr(err, "reasoning_effort") != nil
	&& (strstr(err, "not supported") != nil
	 || strstr(err, "nsupported") != nil)){
		switch(c->reasonquirk){
		case Reffort:
			if(c->thinkmode == Thinkadaptive
			&& c->effort != nil && c->effort[0] != '\0'){
				c->reasonquirk = Romit;
				return 1;
			}
			c->reasonquirk = Rnone;
			return 1;
		case Romit:
			c->reasonquirk = Rnone;
			return 1;
		case Rnone:
			c->reasonquirk = Rdead;
			return 0;
		}
		return 0;	/* Rdead */
	}
	return 0;
}

/*
 * Build the OpenAI tools array: same schema payload as Anthropic's
 * input_schema, wrapped differently.
 */
static Json*
mkopenatools(void)
{
	Json *arr, *t, *fn;
	Tooldef *td;
	int i;

	arr = jarray();
	for(i = 0; (td = tooldef(i)) != nil; i++){
		fn = jobject();
		jset(fn, "name", jstring(td->name));
		jset(fn, "description", jstring(td->desc));
		jset(fn, "parameters", toolschema(td));

		t = jobject();
		jset(t, "type", jstring("function"));
		jset(t, "function", fn);
		jappend(arr, t);
	}
	return arr;
}

/*
 * Append messages derived from a neutral content array for an
 * assistant turn.  Concatenates text blocks into one
 * "content" string, collects tool_use blocks into "tool_calls".
 * Skips thinking and redacted_thinking blocks entirely (they
 * have no OpenAI equivalent and are not replayed here).
 * Appends a single assistant message to msgs.
 */
static void
appendassistantmsg(Json *msgs, Json *content)
{
	Json *block, *msg, *toolcalls, *tc, *fn;
	char *btype, *text, *bid, *bname, *argstr;
	Json *binput;
	Fmt f;
	int i, havetc;

	fmtstrinit(&f);
	toolcalls = jarray();
	havetc = 0;

	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil)
			continue;
		if(strcmp(btype, "text") == 0){
			text = jstr(block, "text");
			if(text != nil && !blankstr(text))
				fmtprint(&f, "%s", text);
		} else if(strcmp(btype, "tool_use") == 0){
			bid = jstr(block, "id");
			bname = jstr(block, "name");
			binput = jget(block, "input");
			/*
			 * arguments must be a JSON string, not an
			 * object; serialize the input object.
			 */
			if(binput != nil)
				argstr = jsonstr(binput);
			else
				argstr = nil;

			fn = jobject();
			jset(fn, "name", jstring(bname ? bname : ""));
			jset(fn, "arguments", jstring(argstr ? argstr : "{}"));
			free(argstr);

			tc = jobject();
			jset(tc, "id", jstring(bid ? bid : ""));
			jset(tc, "type", jstring("function"));
			jset(tc, "function", fn);
			jappend(toolcalls, tc);
			havetc = 1;
		}
		/* thinking and redacted_thinking: skip */
	}

	msg = jobject();
	jset(msg, "role", jstring("assistant"));
	text = fmtstrflush(&f);
	jset(msg, "content", jstring(text ? text : ""));
	free(text);
	if(havetc)
		jset(msg, "tool_calls", toolcalls);
	else
		jsonfree(toolcalls);
	jappend(msgs, msg);
}

/*
 * Append messages derived from a neutral content array for a
 * user (tool-results) turn.  Each tool_result block becomes its
 * own role:"tool" message.  Non-blank text blocks are collected
 * into a final role:"user" message.
 */
static void
appendtoolresultmsgs(Json *msgs, Json *content)
{
	Json *block, *msg;
	char *btype, *id, *text;
	Fmt f;
	int i, hastext;

	fmtstrinit(&f);
	hastext = 0;

	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil)
			continue;
		if(strcmp(btype, "tool_result") == 0){
			id = jstr(block, "tool_use_id");
			text = jstr(block, "content");
			msg = jobject();
			jset(msg, "role", jstring("tool"));
			jset(msg, "tool_call_id", jstring(id ? id : ""));
			jset(msg, "content", jstring(text ? text : ""));
			jappend(msgs, msg);
		} else if(strcmp(btype, "text") == 0){
			text = jstr(block, "text");
			if(text != nil && !blankstr(text)){
				fmtprint(&f, "%s", text);
				hastext = 1;
			}
		}
		/* other block types: skip */
	}

	if(hastext){
		text = fmtstrflush(&f);
		msg = jobject();
		jset(msg, "role", jstring("user"));
		jset(msg, "content", jstring(text ? text : ""));
		free(text);
		jappend(msgs, msg);
	} else {
		char *s;
		s = fmtstrflush(&f);
		free(s);
	}
}

/*
 * Build the OpenAI Chat Completions request JSON from the
 * conversation's neutral content form.  "stream":true is added
 * by sendonce; stream_options here ensures usage is included
 * in the final chunk.
 *
 * The output-token cap is max_completion_tokens by default:
 * OpenAI deprecated max_tokens, every current model there
 * accepts the new name, and reasoning-era models (o-series,
 * gpt-5 family) reject the old one outright.  The choice is
 * per-server, not per-model, so a single Conv.oldmaxtok flag
 * switches back to the legacy max_tokens spelling for older
 * openai-compatible servers; openaiquirk sets it automatically
 * when such a server complains (current llama.cpp and vllm
 * both accept the new name, so the fallback is rarely needed).
 *
 * Thinkbudget has no OpenAI equivalent (the OpenAI API has no
 * budget_tokens concept); it is silently ignored when talking
 * to an OpenAI provider.
 */
Json*
openaibuildreq(Conv *c)
{
	Json *req, *msgs, *msg, *content, *streamopts;
	Msg *m;

	req = jobject();
	jset(req, "model", jstring(c->model));
	/* output-token cap; field name is a server quirk, see above */
	if(c->oldmaxtok)
		jset(req, "max_tokens", jintval(c->maxtokens));
	else
		jset(req, "max_completion_tokens", jintval(c->maxtokens));

	/* request usage in the final chunk */
	streamopts = jobject();
	jset(streamopts, "include_usage", jbool(1));
	jset(req, "stream_options", streamopts);

	/*
	 * Thinkadaptive: map effort to reasoning_effort.
	 * Thinkbudget: no OpenAI equivalent, silently ignored.
	 *
	 * The quirk ladder (Conv.reasonquirk, advanced by
	 * openaiquirk when the server rejects tools+reasoning)
	 * overrides the normal mapping: Rnone sends an explicit
	 * "none" -- required when the server applies a default
	 * reasoning effort for the model even though we never sent
	 * the field -- and Romit/Rdead suppress the field entirely.
	 */
	if(c->reasonquirk == Rnone)
		jset(req, "reasoning_effort", jstring("none"));
	else if(c->reasonquirk == Reffort
	&& c->thinkmode == Thinkadaptive
	&& c->effort != nil && c->effort[0] != '\0')
		jset(req, "reasoning_effort", jstring(c->effort));

	/*
	 * Build the messages array.  System prompt is first if set.
	 * Then walk the neutral conversation, converting each Msg.
	 */
	msgs = jarray();

	if(c->sysprompt != nil && c->sysprompt[0] != '\0'){
		msg = jobject();
		jset(msg, "role", jstring("system"));
		jset(msg, "content", jstring(c->sysprompt));
		jappend(msgs, msg);
	}

	for(m = c->msgs; m != nil; m = m->next){
		if(m->rawjson == nil){
			/* plain text message */
			if(m->role == Muser){
				char *text;

				text = (blankstr(m->text)) ? "(no text)" : m->text;
				msg = jobject();
				jset(msg, "role", jstring("user"));
				jset(msg, "content", jstring(text));
				jappend(msgs, msg);
			} else {
				/* Massistant plain text */
				msg = jobject();
				jset(msg, "role", jstring("assistant"));
				jset(msg, "content",
					jstring(m->text ? m->text : ""));
				jappend(msgs, msg);
			}
			continue;
		}

		/* rawjson: neutral content array */
		content = jsonparse(m->rawjson);
		if(content == nil){
			/*
			 * Parse failure: fall back to plain text.
			 * This can only happen with a corrupt snapshot;
			 * the tool protocol may be broken, but we do
			 * the best we can rather than refusing to send.
			 */
			fprint(2, "openai: stored message failed to reparse: %r\n");
			if(m->role == Massistant){
				msg = jobject();
				jset(msg, "role", jstring("assistant"));
				jset(msg, "content",
					jstring(m->text ? m->text : ""));
				jappend(msgs, msg);
			} else {
				char *text;

				text = (blankstr(m->text)) ? "(no text)" : m->text;
				msg = jobject();
				jset(msg, "role", jstring("user"));
				jset(msg, "content", jstring(text));
				jappend(msgs, msg);
			}
			continue;
		}

		if(m->role == Massistant){
			appendassistantmsg(msgs, content);
		} else {
			/*
			 * Muser with rawjson: tool results (and possibly
			 * text blocks) from the previous tool round.
			 */
			appendtoolresultmsgs(msgs, content);
		}
		jsonfree(content);
	}

	jset(req, "messages", msgs);
	jset(req, "tools", mkopenatools());
	return req;
}

/*
 * Per-index accumulator for a streaming tool_call.  The id and
 * name arrive once in the first fragment; arguments accumulate
 * as string fragments across chunks.
 */
typedef struct Stool Stool;
struct Stool {
	int used;
	char *id;
	char *name;
	Sbuf args;	/* accumulates function.arguments fragments */
};

enum {
	Maxstools = 64,
};

/*
 * Free all Stool accumulators.
 */
static void
freestools(Stool *stools, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(stools[i].id);
		free(stools[i].name);
		free(stools[i].args.s);
	}
}

/*
 * Build a Reply from the accumulated text and tool-call data,
 * parallel to blocks2reply in claude.c.  The rawjson is the
 * neutral content array: a text block if non-blank, followed
 * by tool_use blocks in index order.  Stop reason is
 * normalized to Anthropic names.
 */
static Reply*
buildreply(Sbuf *textbuf, Stool *stools, int nstools, char *finishreason)
{
	Reply *r;
	Json *content, *block, *input;
	ToolCall *head, *tail, *tc;
	Tooldef *td;
	char *stopname;
	int i;

	r = emallocz(sizeof *r, 1);
	content = jarray();

	/* text block */
	if(!blankstr(textbuf->s)){
		block = jobject();
		jset(block, "type", jstring("text"));
		jset(block, "text", jstring(textbuf->s));
		jappend(content, block);
		r->text = estrdup(textbuf->s);
	} else {
		r->text = estrdup("");
	}

	/* tool_use blocks and ToolCall chain */
	head = tail = nil;
	for(i = 0; i < nstools; i++){
		if(!stools[i].used)
			continue;

		block = jobject();
		jset(block, "type", jstring("tool_use"));
		jset(block, "id",
			jstring(stools[i].id ? stools[i].id : ""));
		jset(block, "name",
			jstring(stools[i].name ? stools[i].name : ""));

		/*
		 * Parse the accumulated arguments string back into a
		 * JSON object for the neutral form; fall back to an
		 * empty object if empty or unparseable.
		 */
		if(stools[i].args.len > 0)
			input = jsonparse(stools[i].args.s);
		else
			input = nil;
		if(input == nil)
			input = jobject();
		jset(block, "input", input);
		jappend(content, block);

		td = findtool(stools[i].name);
		tc = emallocz(sizeof *tc, 1);
		tc->id = estrdup(stools[i].id ? stools[i].id : "");
		tc->name = estrdup(stools[i].name ? stools[i].name : "");
		tc->type = td != nil ? td->type : -1;
		parseinput(tc, td, input);
		if(tail == nil) head = tc;
		else tail->next = tc;
		tail = tc;
	}

	r->rawjson = jsonstr(content);
	jsonfree(content);
	r->tools = head;

	/*
	 * Normalize finish_reason to Anthropic stop-reason names,
	 * as required by the provider contract (see PROVIDERS.md
	 * and the readstream slot comment in claudeimpl.h).
	 *   tool_calls -> tool_use
	 *   length     -> max_tokens
	 *   stop (and anything else) -> end_turn
	 */
	if(finishreason != nil && strcmp(finishreason, "tool_calls") == 0)
		stopname = "tool_use";
	else if(finishreason != nil && strcmp(finishreason, "length") == 0)
		stopname = "max_tokens";
	else
		stopname = "end_turn";

	r->stopped = (strcmp(stopname, "tool_use") != 0);
	return r;
}

/*
 * OpenAI: consume a streamed SSE response and reassemble the
 * assistant turn into a Reply.  Chunks carry delta.content for
 * text and delta.tool_calls[] for tool calls; each tool call
 * entry has an "index" field identifying which call it belongs
 * to.  The stream is terminated by "data: [DONE]".
 */
Reply*
openaireadstream(Conv *c, Biobuf *bp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	char *line, *p, *finishreason, *errmsg;
	Json *chunk, *choices, *choice, *delta, *tcarr, *tcentry;
	Json *ucobj, *details;
	Sbuf textbuf;
	Stool stools[Maxstools];
	int nstools, done, err, idx;
	Reply *r;
	char *s;
	int i;

	USED(c);
	memset(&textbuf, 0, sizeof textbuf);
	memset(stools, 0, sizeof stools);
	nstools = 0;
	finishreason = nil;
	done = 0;
	err = 0;

	while(!done && (line = Brdstr(bp, '\n', 1)) != nil){
		if(strncmp(line, "data:", 5) != 0){
			free(line);
			continue;
		}
		p = line + 5;
		while(*p == ' ')
			p++;
		if(*p == '\0'){
			free(line);
			continue;
		}

		/* terminal marker */
		if(strcmp(p, "[DONE]") == 0){
			free(line);
			done = 1;
			break;
		}

		chunk = jsonparse(p);
		free(line);
		if(chunk == nil)
			continue;

		/* error object in the chunk */
		errmsg = jstr(jget(chunk, "error"), "message");
		if(errmsg != nil){
			werrstr("API error: %s", errmsg);
			jsonfree(chunk);
			err = 1;
			break;
		}

		/* usage (present in the final chunk when include_usage:true) */
		ucobj = jget(chunk, "usage");
		if(ucobj != nil && ucobj->type == Jobject && usage != nil){
			usage->input_tokens += (int)jint(ucobj, "prompt_tokens");
			usage->output_tokens += (int)jint(ucobj, "completion_tokens");
			/* cached tokens nested under prompt_tokens_details */
			details = jget(ucobj, "prompt_tokens_details");
			if(details != nil && details->type == Jobject)
				usage->cache_read_input_tokens +=
					(int)jint(details, "cached_tokens");
			/* no cache_creation equivalent for OpenAI */
		}

		/* choices[0].delta */
		choices = jget(chunk, "choices");
		if(choices == nil || choices->type != Jarray || choices->nitem == 0){
			jsonfree(chunk);
			continue;
		}
		choice = jidx(choices, 0);

		/* finish_reason: non-null once at end */
		s = jstr(choice, "finish_reason");
		if(s != nil && s[0] != '\0'){
			free(finishreason);
			finishreason = estrdup(s);
		}

		delta = jget(choice, "delta");
		if(delta == nil){
			jsonfree(chunk);
			continue;
		}

		/* delta.content: text fragment */
		s = jstr(delta, "content");
		if(s != nil && s[0] != '\0'){
			sbappend(&textbuf, s, strlen(s));
			if(cb != nil)
				cb(s, aux);
		}

		/* delta.tool_calls: array of per-index fragments */
		tcarr = jget(delta, "tool_calls");
		if(tcarr != nil && tcarr->type == Jarray){
			for(i = 0; i < tcarr->nitem; i++){
				tcentry = jidx(tcarr, i);
				if(tcentry == nil)
					continue;
				idx = (int)jint(tcentry, "index");
				if(idx < 0 || idx >= Maxstools){
					werrstr("tool call index %d exceeds limit (%d)",
						idx, Maxstools);
					jsonfree(chunk);
					err = 1;
					goto out;
				}
				if(idx >= nstools)
					nstools = idx + 1;
				stools[idx].used = 1;

				/* id and name arrive in the first fragment */
				s = jstr(tcentry, "id");
				if(s != nil && s[0] != '\0'){
					free(stools[idx].id);
					stools[idx].id = estrdup(s);
				}
				s = jstr(jget(tcentry, "function"), "name");
				if(s != nil && s[0] != '\0'){
					free(stools[idx].name);
					stools[idx].name = estrdup(s);
				}

				/* arguments accumulate as string fragments */
				s = jstr(jget(tcentry, "function"), "arguments");
				if(s != nil && s[0] != '\0')
					sbappend(&stools[idx].args, s, strlen(s));
			}
		}

		jsonfree(chunk);
	}

out:
	if(!done && !err){
		werrstr("response stream ended unexpectedly (connection lost?)");
		err = 1;
	}

	if(err){
		free(textbuf.s);
		freestools(stools, Maxstools);
		free(finishreason);
		return nil;
	}

	r = buildreply(&textbuf, stools, nstools, finishreason);

	/* record normalized stop reason in usage */
	if(usage != nil){
		char *stopname;

		if(finishreason != nil && strcmp(finishreason, "tool_calls") == 0)
			stopname = "tool_use";
		else if(finishreason != nil && strcmp(finishreason, "length") == 0)
			stopname = "max_tokens";
		else
			stopname = "end_turn";
		free(usage->stop_reason);
		usage->stop_reason = estrdup(stopname);
	}

	free(textbuf.s);
	freestools(stools, Maxstools);
	free(finishreason);
	return r;
}
