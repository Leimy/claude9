/*
 * OpenAI Responses API provider for claude9.
 *
 * Implements the "responses" entry of the provider vtable (see
 * claudeimpl.h and PROVIDERS.md): request assembly from the
 * neutral conversation form, and streamed response parsing back
 * into a Reply carrying neutral rawjson.  Auth is the same
 * bearer token as Chat Completions (openaiheaders in openai.c).
 *
 * Why this exists at all: OpenAI's newer reasoning models
 * refuse function tools on /v1/chat/completions whenever
 * reasoning is in effect -- "Function tools with
 * reasoning_effort are not supported for <model> in
 * /v1/chat/completions.  To use function tools, use
 * /v1/responses or set reasoning_effort to 'none'" -- and some
 * of them (gpt-6-astra, live) do not accept 'none' at all, so
 * the Chat Completions quirk ladder in openai.c has nowhere to
 * go.  Tools are load-bearing for this program, so the answer
 * is to speak the endpoint the server asks for.  openaiquirk
 * moves a session here automatically (switchresponses); a
 * session can also select it directly (provider file).
 *
 * Wire format cheat sheet (request):
 *   model, instructions (system prompt), max_output_tokens,
 *   input: [items], tools: [{type:function, name, description,
 *   parameters}], reasoning: {effort, summary}, store: false,
 *   include: ["reasoning.encrypted_content"], stream: true.
 * Input items: {role:user, content:"..."} messages,
 *   {type:message, role:assistant, content:[{type:output_text,
 *   text}]}, {type:function_call, id, call_id, name, arguments},
 *   {type:function_call_output, call_id, output}, and replayed
 *   {type:reasoning, id, summary, encrypted_content} items.
 * Stream: SSE "data:" lines, each a JSON object with a "type":
 *   response.output_text.delta, response.reasoning_summary_
 *   text.delta, response.output_item.done (a complete item),
 *   response.completed / response.incomplete / response.failed
 *   (the whole response, with output[] and usage), and error.
 *   No [DONE] marker; the terminal event ends the stream.
 *
 * Conversation state is client-managed (store:false): every
 * request replays the whole history, as with the other
 * providers.  Reasoning models bind each reasoning item to the
 * items that followed it in the same turn, so the turn is
 * replayed with the server's own item ids and its reasoning
 * items (encrypted, since nothing is stored server-side); see
 * the neutral-form note in claudeimpl.h.  Servers that reject
 * any of the optional parts are handled by responsesquirk.
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include "json.h"
#include "claude.h"
#include "claudeimpl.h"

/*
 * Tools array, Responses shape: flat {type, name, description,
 * parameters}, not nested under "function" as Chat Completions
 * has it.
 */
static Json*
mkresptools(void)
{
	Json *arr, *t;
	Tooldef *td;
	int i;

	arr = jarray();
	for(i = 0; (td = tooldef(i)) != nil; i++){
		t = jobject();
		jset(t, "type", jstring("function"));
		jset(t, "name", jstring(td->name));
		jset(t, "description", jstring(td->desc));
		jset(t, "parameters", toolschema(td));
		jappend(arr, t);
	}
	return arr;
}

/*
 * Append input items for one assistant turn's neutral content
 * array, preserving block order (a reasoning item must stay
 * immediately before the item it produced).
 *
 *   text       -> {type:message, role:assistant,
 *                  content:[{type:output_text, text}]}
 *   tool_use   -> {type:function_call, call_id, name, arguments}
 *   reasoning  -> the stored item, verbatim
 *
 * Item ids (block "item_id"; the reasoning item's own "id")
 * are replayed unless Rqnoreasonitems is set, in which case
 * reasoning items are dropped too: without them the ids would
 * point at items the server can no longer pair up.  Anthropic-
 * only blocks (thinking, redacted_thinking, server_tool_use,
 * advisor_tool_result) have no Responses equivalent and are
 * skipped, as openai.c does.
 */
static void
appendassistantitems(Conv *c, Json *input, Json *content)
{
	Json *block, *item, *parts, *part, *binput;
	char *btype, *text, *s;
	int i, replayids;

	replayids = !(c->respquirks & Rqnoreasonitems);
	for(i = 0; i < content->nitem; i++){
		block = content->items[i];
		btype = jstr(block, "type");
		if(btype == nil)
			continue;
		if(strcmp(btype, "text") == 0){
			text = jstr(block, "text");
			if(text == nil || blankstr(text))
				continue;
			part = jobject();
			jset(part, "type", jstring("output_text"));
			jset(part, "text", jstring(text));
			jset(part, "annotations", jarray());
			parts = jarray();
			jappend(parts, part);
			item = jobject();
			jset(item, "type", jstring("message"));
			s = jstr(block, "item_id");
			if(replayids && s != nil && s[0] != '\0')
				jset(item, "id", jstring(s));
			jset(item, "role", jstring("assistant"));
			jset(item, "content", parts);
			jappend(input, item);
		} else if(strcmp(btype, "tool_use") == 0){
			item = jobject();
			jset(item, "type", jstring("function_call"));
			s = jstr(block, "item_id");
			if(replayids && s != nil && s[0] != '\0')
				jset(item, "id", jstring(s));
			s = jstr(block, "id");
			jset(item, "call_id", jstring(s ? s : ""));
			s = jstr(block, "name");
			jset(item, "name", jstring(s ? s : ""));
			binput = jget(block, "input");
			s = binput != nil ? jsonstr(binput) : nil;
			jset(item, "arguments", jstring(s ? s : "{}"));
			free(s);
			jappend(input, item);
		} else if(strcmp(btype, "reasoning") == 0){
			if(!replayids)
				continue;
			item = jcopy(block);
			if(item != nil)
				jappend(input, item);
		}
	}
}

/*
 * Append input items for one user turn's neutral content array:
 * each tool_result becomes a function_call_output item (they
 * come first in the neutral form, as the protocol wants), and
 * any non-blank text blocks are joined into one trailing user
 * message.
 */
static void
appenduseritems(Json *input, Json *content)
{
	Json *block, *item;
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
			item = jobject();
			jset(item, "type", jstring("function_call_output"));
			jset(item, "call_id", jstring(id ? id : ""));
			jset(item, "output", jstring(text ? text : ""));
			jappend(input, item);
		} else if(strcmp(btype, "text") == 0){
			text = jstr(block, "text");
			if(text != nil && !blankstr(text)){
				if(hastext)
					fmtprint(&f, "\n");
				fmtprint(&f, "%s", text);
				hastext = 1;
			}
		}
	}
	text = fmtstrflush(&f);
	if(hastext){
		item = jobject();
		jset(item, "role", jstring("user"));
		jset(item, "content", jstring(text ? text : ""));
		jappend(input, item);
	}
	free(text);
}

/*
 * Build the Responses API request from the conversation's
 * neutral form.  "stream":true is added by sendonce.
 *
 * The system prompt goes in "instructions" rather than a
 * system/developer message.  Reasoning is requested only when
 * the session has an explicit effort, exactly as the Chat
 * Completions provider does with reasoning_effort; the summary
 * is asked for so reasoning text can be streamed between
 * [thinking] markers like Anthropic's, except with effort
 * "none", where there is nothing to summarize.  Thinkbudget has
 * no equivalent here and is ignored.
 *
 * store:false keeps the conversation client-side (no 30-day
 * server retention of every turn), which in turn requires
 * include:["reasoning.encrypted_content"] so the reasoning
 * items come back in a form that can be replayed.  Both are
 * omitted after a server rejects them (Rqnoinclude); the
 * server then keeps the items itself and resolves replayed
 * reasoning ids against its own store.
 */
Json*
responsesbuildreq(Conv *c)
{
	Json *req, *input, *neutral, *nmsg, *content, *inc, *reason;
	char *role;
	int i;

	req = jobject();
	jset(req, "model", jstring(c->model));
	jset(req, "max_output_tokens", jintval(c->maxtokens));
	if(c->sysprompt != nil && c->sysprompt[0] != '\0')
		jset(req, "instructions", jstring(c->sysprompt));

	if(!(c->respquirks & Rqnoinclude)){
		jset(req, "store", jbool(0));
		inc = jarray();
		jappend(inc, jstring("reasoning.encrypted_content"));
		jset(req, "include", inc);
	}

	if(c->effort != nil && c->effort[0] != '\0'){
		reason = jobject();
		jset(reason, "effort", jstring(c->effort));
		if(!(c->respquirks & Rqnosummary)
		&& strcmp(c->effort, "none") != 0)
			jset(reason, "summary", jstring("auto"));
		jset(req, "reasoning", reason);
	}

	input = jarray();
	neutral = neutralmessages(c);
	for(i = 0; i < neutral->nitem; i++){
		nmsg = neutral->items[i];
		role = jstr(nmsg, "role");
		content = jget(nmsg, "content");
		if(content == nil || content->type != Jarray)
			continue;
		if(role != nil && strcmp(role, "assistant") == 0)
			appendassistantitems(c, input, content);
		else
			appenduseritems(input, content);
	}
	jsonfree(neutral);

	jset(req, "input", input);
	jset(req, "tools", mkresptools());
	return req;
}

/*
 * Provider quirk hook (see Provider.quirk in claudeimpl.h):
 * each recognized complaint turns off one optional part of the
 * request (a bit in Conv.respquirks) and asks sendonce to
 * resend.  Bits are only ever set, so this terminates: at most
 * three retries over the life of a Conv.
 */
static int
badparam(char *err)
{
	return strstr(err, "not supported") != nil
		|| strstr(err, "nsupported") != nil
		|| strstr(err, "nrecognized") != nil
		|| strstr(err, "nknown") != nil
		|| strstr(err, "nvalid") != nil
		|| strstr(err, "not permitted") != nil;
}

int
responsesquirk(Conv *c, char *err)
{
	if(err == nil)
		return 0;
	/*
	 * A compatible server that does not implement client-side
	 * state: "Unknown parameter: 'include'", "'store' is not
	 * supported", or a complaint about the encrypted_content
	 * include value itself.
	 */
	if(!(c->respquirks & Rqnoinclude)
	&& (strstr(err, "include") != nil || strstr(err, "'store'") != nil
	 || strstr(err, "encrypted_content") != nil)
	&& badparam(err)){
		c->respquirks |= Rqnoinclude;
		return 1;
	}
	/* reasoning summaries are gated per model/org on OpenAI */
	if(!(c->respquirks & Rqnosummary)
	&& strstr(err, "summary") != nil && badparam(err)){
		c->respquirks |= Rqnosummary;
		return 1;
	}
	/*
	 * The server cannot pair our replayed reasoning items with
	 * their following items ("Item 'rs_...' of type 'reasoning'
	 * was provided without its required following item", or the
	 * mirror image for a function_call), or does not accept
	 * reasoning items in input at all.  Replay neither the items
	 * nor the ids from now on; the model re-derives its reasoning
	 * after each tool result instead, which costs tokens but
	 * works.
	 */
	if(!(c->respquirks & Rqnoreasonitems)
	&& strstr(err, "reasoning") != nil
	&& (strstr(err, "item") != nil || strstr(err, "Item") != nil)){
		c->respquirks |= Rqnoreasonitems;
		return 1;
	}
	return 0;
}

/*
 * Build a Reply from the response's output items.  The neutral
 * content array keeps the items' order: text blocks (one per
 * message item, its output_text/refusal parts concatenated),
 * tool_use blocks for function_call items, and reasoning items
 * verbatim.  Stop reason is normalized to the Anthropic names.
 */
static Reply*
items2reply(Json *items, char *status, char *reason, Usage *usage)
{
	Reply *r;
	Json *content, *block, *item, *parts, *part, *input;
	ToolCall *head, *tail, *tc;
	Tooldef *td;
	Fmt f, tf;
	char *itype, *ptype, *s, *text, *stopname;
	int i, j, ntools;

	r = emallocz(sizeof *r, 1);
	content = jarray();
	fmtstrinit(&f);
	head = tail = nil;
	ntools = 0;

	for(i = 0; i < items->nitem; i++){
		item = items->items[i];
		if(item == nil || item->type != Jobject)
			continue;
		itype = jstr(item, "type");
		if(itype == nil)
			continue;
		if(strcmp(itype, "message") == 0){
			fmtstrinit(&tf);
			parts = jget(item, "content");
			if(parts != nil && parts->type == Jarray){
				for(j = 0; j < parts->nitem; j++){
					part = parts->items[j];
					ptype = jstr(part, "type");
					if(ptype == nil)
						continue;
					if(strcmp(ptype, "output_text") == 0
					|| strcmp(ptype, "text") == 0)
						s = jstr(part, "text");
					else if(strcmp(ptype, "refusal") == 0)
						s = jstr(part, "refusal");
					else
						s = nil;
					if(s != nil)
						fmtprint(&tf, "%s", s);
				}
			}
			text = fmtstrflush(&tf);
			if(!blankstr(text)){
				block = jobject();
				jset(block, "type", jstring("text"));
				jset(block, "text", jstring(text));
				s = jstr(item, "id");
				if(s != nil && s[0] != '\0')
					jset(block, "item_id", jstring(s));
				jappend(content, block);
				fmtprint(&f, "%s", text);
			}
			free(text);
		} else if(strcmp(itype, "function_call") == 0){
			block = jobject();
			jset(block, "type", jstring("tool_use"));
			s = jstr(item, "call_id");
			jset(block, "id", jstring(s ? s : ""));
			s = jstr(item, "name");
			jset(block, "name", jstring(s ? s : ""));
			s = jstr(item, "arguments");
			input = (s != nil && s[0] != '\0') ? jsonparse(s) : nil;
			if(input == nil || input->type != Jobject){
				jsonfree(input);
				input = jobject();
			}
			jset(block, "input", input);
			s = jstr(item, "id");
			if(s != nil && s[0] != '\0')
				jset(block, "item_id", jstring(s));
			jappend(content, block);

			/*
			 * Always a ToolCall, even for an unknown tool
			 * name: every function_call needs a
			 * function_call_output next turn, or the server
			 * rejects the conversation.
			 */
			td = findtool(jstr(item, "name"));
			tc = emallocz(sizeof *tc, 1);
			s = jstr(item, "call_id");
			tc->id = estrdup(s ? s : "");
			s = jstr(item, "name");
			tc->name = estrdup(s ? s : "");
			tc->type = td != nil ? td->type : -1;
			parseinput(tc, td, input);
			if(tail == nil) head = tc;
			else tail->next = tc;
			tail = tc;
			ntools++;
		} else if(strcmp(itype, "reasoning") == 0){
			block = jcopy(item);
			if(block != nil)
				jappend(content, block);
		}
		/* built-in tool calls etc.: never requested, skip */
	}

	r->text = fmtstrflush(&f);
	r->rawjson = jsonstr(content);
	jsonfree(content);
	r->tools = head;

	/*
	 * status "incomplete" with reason max_output_tokens is the
	 * guillotine; otherwise the turn ended either asking for
	 * tools or on its own.
	 */
	if(status != nil && strcmp(status, "incomplete") == 0
	&& reason != nil && strcmp(reason, "max_output_tokens") == 0)
		stopname = "max_tokens";
	else if(ntools > 0)
		stopname = "tool_use";
	else
		stopname = "end_turn";
	r->stopped = strcmp(stopname, "tool_use") != 0;
	if(usage != nil){
		free(usage->stop_reason);
		usage->stop_reason = estrdup(stopname);
	}
	return r;
}

/*
 * Consume a streamed Responses SSE body and reassemble the
 * turn.  Text and reasoning-summary deltas are shown live via
 * cb; the Reply itself is built from complete items -- each
 * response.output_item.done as it arrives, superseded by the
 * terminal event's response.output when present -- rather than
 * by reassembling fragments, since the API hands us the whole
 * item anyway.
 */
Reply*
responsesreadstream(Conv *c, Biobuf *bp, Usage *usage,
	void (*cb)(char*, void*), void *aux)
{
	char *line, *p, *etype, *s, *status, *reason;
	Json *ev, *item, *items, *resp, *out, *uobj, *det;
	int done, err, inthink;
	Reply *r;

	items = jarray();
	status = nil;
	reason = nil;
	done = 0;
	err = 0;
	inthink = 0;

	while(!done && !cancelled(c) && (line = Brdstr(bp, '\n', 1)) != nil){
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
		/* not part of this API, but harmless if a proxy adds it */
		if(strcmp(p, "[DONE]") == 0){
			free(line);
			break;
		}
		ev = jsonparse(p);
		free(line);
		if(ev == nil)
			continue;
		etype = jstr(ev, "type");
		if(etype == nil){
			jsonfree(ev);
			continue;
		}

		if(strcmp(etype, "error") == 0){
			s = jstr(ev, "message");
			if(s == nil)
				s = jstr(jget(ev, "error"), "message");
			werrstr("API error: %s", s ? s : "unknown");
			jsonfree(ev);
			err = 1;
			break;
		}

		if(strcmp(etype, "response.output_text.delta") == 0){
			s = jstr(ev, "delta");
			if(s != nil && s[0] != '\0' && cb != nil)
				cb(s, aux);
		} else if(strcmp(etype, "response.reasoning_summary_text.delta") == 0){
			s = jstr(ev, "delta");
			if(s != nil && s[0] != '\0' && cb != nil){
				if(!inthink){
					cb("[thinking]\n", aux);
					inthink = 1;
				}
				cb(s, aux);
			}
		} else if(strcmp(etype, "response.output_item.done") == 0){
			item = jget(ev, "item");
			if(item != nil && item->type == Jobject){
				s = jstr(item, "type");
				if(inthink && cb != nil
				&& s != nil && strcmp(s, "reasoning") == 0){
					cb("\n[/thinking]\n", aux);
					inthink = 0;
				}
				item = jcopy(item);
				if(item != nil)
					jappend(items, item);
			}
		} else if(strcmp(etype, "response.completed") == 0
		|| strcmp(etype, "response.incomplete") == 0
		|| strcmp(etype, "response.failed") == 0){
			resp = jget(ev, "response");
			if(strcmp(etype, "response.failed") == 0){
				s = jstr(jget(resp, "error"), "message");
				werrstr("API error: %s", s ? s : "response failed");
				jsonfree(ev);
				err = 1;
				break;
			}
			s = jstr(resp, "status");
			free(status);
			status = estrdup(s ? s : "completed");
			s = jstr(jget(resp, "incomplete_details"), "reason");
			free(reason);
			reason = s != nil ? estrdup(s) : nil;

			uobj = jget(resp, "usage");
			if(uobj != nil && uobj->type == Jobject && usage != nil){
				usage->input_tokens += (int)jint(uobj, "input_tokens");
				usage->output_tokens += (int)jint(uobj, "output_tokens");
				det = jget(uobj, "input_tokens_details");
				if(det != nil && det->type == Jobject)
					usage->cache_read_input_tokens +=
						(int)jint(det, "cached_tokens");
			}

			/* the final output array is authoritative */
			out = jget(resp, "output");
			if(out != nil && out->type == Jarray && out->nitem > 0){
				out = jcopy(out);
				if(out != nil){
					jsonfree(items);
					items = out;
				}
			}
			done = 1;
		}
		jsonfree(ev);
	}

	if(inthink && cb != nil)
		cb("\n[/thinking]\n", aux);

	if(!done && !err){
		if(cancelled(c))
			werrstr(Cancelmsg);
		else
			werrstr("response stream ended unexpectedly (connection lost?)");
		err = 1;
	}
	if(err){
		jsonfree(items);
		free(status);
		free(reason);
		return nil;
	}

	r = items2reply(items, status, reason, usage);
	jsonfree(items);
	free(status);
	free(reason);
	return r;
}
