# Multi-provider support: Anthropic + OpenAI-compatible APIs

Status: implementation and README documentation are complete for the
current provider interface.  ./tests previously passed (199+ tests; three
test bugs found and fixed on the first runs: a msgnew(nil)
crash, a jint-on-Jbool assertion, and a canned SSE transcript
missing a closing brace).  Also done since: per-provider
models listing, claude9fs -P default-provider flag with
per-provider key requirements and default model, claudetalk
/provider + /baseurl commands and -P option.  First live test
against real OpenAI hit the max_tokens rejection; fixed with
the Provider.quirk hook (max_completion_tokens default plus
automatic legacy fallback).  Second live test (against a
"gpt-5.6-sol" reasoning model through an openai-compatible
endpoint) hit a second request-shape rejection: the server
refuses reasoning_effort when function tools are present in
the same request ("Function tools with reasoning_effort are
not supported for <model> ... To use function tools, do not
set reasoning_effort").  This case took THREE tries to get
right (see "The reasoning_effort saga" below): first an
omit-latch (Conv.noreasoning), then a guard against retrying
with a byte-identical request, and finally -- after a fourth
live test showed the error fires even on a fresh Thinkoff
session that never sent the field at all -- the realization
that the server applies a DEFAULT reasoning effort when the
field is absent, so omitting the field can never fix
anything; the correct move is an explicit
reasoning_effort:"none" to override the server-side default.
Now implemented as a monotonic quirk ladder (Conv.reasonquirk:
Reffort -> Romit -> Rnone -> Rdead), with sendonce allowing up
to Maxquirks retries per round so the ladder can climb within
a single prompt.  Tools stay in the request throughout (tools
are load-bearing for this program; reasoning_effort is a
nice-to-have).  Regression tests: topenaiquirkreasoning
(Thinkadaptive start, full ladder) and
topenaiquirkreasoningoff (Thinkoff start, the live case,
skips straight to "none").  README now documents providers,
keys, provider/baseurl files, and claudetalk controls.  Still not
done: live confirmation that "none" satisfies the gpt-5.6-sol
endpoint, more live testing (tool rounds against real OpenAI and
a compat server), and the gaps below.
Last updated: after README reconciliation and review remediation.

## The reasoning_effort saga: server-side default reasoning

This one took three fixes to get right; the history matters
because each wrong theory looked plausible at the time.

Live symptom (from /dev/snarf, running claudetalk against an
openai-compatible endpoint with model "gpt-5.6-sol" and
thinking left at its session default of off):

    openai!gpt-5.6-sol/16384> Hello is this working?

    [error: API error: Function tools with reasoning_effort are not supported
    for gpt-5.6-sol in /v1/chat/completions. To use function tool]

Attempt 1 (wrong): treat it like the max_tokens quirk --
latch Conv.noreasoning so openaibuildreq omits
reasoning_effort, and retry.  But a fresh session defaults to
Thinkoff, so the field was never in the failed request; the
retry resent a byte-identical body and failed with the
byte-identical error, showing the user the rejection twice.

Attempt 2 (wrong conclusion, right observation): only retry
when the field would actually change (Thinkadaptive with
effort set); otherwise surface the error immediately.  The
no-op retry was gone, but the write-up concluded that "this
model+server rejects function tools outright, nothing we can
do" -- and the user kept hitting the same error on every
prompt.  Spinning wheels.

The actual root cause: the server applies a DEFAULT reasoning
effort for reasoning-family models when the field is absent.
"Do not set reasoning_effort" is judged server-side, against
the effective value, not against what the client sent.  So
omitting the field can NEVER fix this case; the only way to
turn reasoning off is to send an explicit
reasoning_effort:"none" overriding the default (newer
OpenAI-style models accept "none" for exactly this purpose).
The evidence was there all along: getting a reasoning error
while sending no reasoning field means the reasoning is
coming from the server's side.

Fix (attempt 3, current): Conv.noreasoning replaced by a
monotonic quirk ladder, Conv.reasonquirk, advanced by
openaiquirk one rung per rejection:

    Reffort  normal; send effort iff Thinkadaptive.
             On error: if we sent an effort value -> Romit,
             retry; if the field was absent (the live case)
             -> Rnone directly, retry.
    Romit    omit the field.  On error (absence still
             rejected: default reasoning is on) -> Rnone,
             retry.
    Rnone    send explicit reasoning_effort:"none".  On
             error -> Rdead, no retry.
    Rdead    suppress the field, surface all errors;
             terminal.

Monotonic, so it terminates: at most three retries over the
life of a Conv, no ping-pong.  sendonce was also changed from
retry-once to retry-up-to-Maxquirks(3)-per-round, so the
ladder can climb to a working shape within one prompt instead
of burning one user prompt per rung.  Regression tests:
topenaiquirkreasoning (Thinkadaptive start, walks the whole
ladder) and topenaiquirkreasoningoff (Thinkoff start, skips
straight to Rnone and asserts the retried request actually
differs).  Still needs a live test to confirm the gpt-5.6-sol
endpoint accepts "none".

Also fixed in claudetalk: the note-kill path taken when a
prompt write fails before a round starts (see doprompt's
"session busy" case and the comment at the "echo kill"
line) could itself print a spurious, unsuppressed error --
"/bin/claudetalk:374: > can't create: /proc/<pid>/note: file
does not exist" -- straight to the terminal, from the same
/dev/snarf transcript.  rc opens redirections left to right;
if the streaming reader process had already exited by the
time the kill was attempted (a real race: it can exit on its
own via the Tflush this very kill is meant to trigger), the
first redirection (> /proc/$cpid/note) failed to open before
the second (>[2]/dev/null) had taken effect, so rc's own
"can't create" diagnostic went to the real terminal instead
of being silenced.  Fixed by reordering the redirections so
fd2 is already pointed at /dev/null before the note file open
is attempted.

## Goal

Make claude9 speak both the Anthropic Messages API (current)
and the OpenAI Chat Completions API, selectable per session.
Chat Completions is the de-facto standard implemented by many
backends (llama.cpp, vllm, OpenRouter, actual OpenAI), so this
buys compatibility with lots of servers, not just OpenAI.

Decision: support BOTH providers in one binary (not a hard
swap).  Chat Completions, not the OpenAI Responses API.

## Where the provider-specific code lives today

All of it is in claude.c.  claude9fs.c, json.c, and the tool
execution machinery are provider-neutral already.

Provider-specific (must abstract or duplicate):

1. buildreq() -- builds the Anthropic request JSON.
   - system prompt: top-level "system" field w/ cache_control
   - messages: content ARRAYS with tool_use / tool_result blocks
   - tools schema: {name, description, input_schema}
   - cache_control "ephemeral" markers (prompt caching)
   - thinking config: Thinkbudget (budget_tokens) and
     Thinkadaptive (output_config.effort)

2. SSE parsing -- sseevent(), ssehandle(), sendonce(),
   Sblock, blocks2reply().  Anthropic streams typed events
   (message_start, content_block_start/delta/stop,
   message_delta, message_stop).  Biggest single rewrite.

3. Repair passes -- repairtooluse(), repairtoolresults(),
   striptextblocks().  These enforce Anthropic's invariants
   (every tool_use paired with a tool_result in the NEXT user
   message; no blank text blocks).  OpenAI has analogous but
   different invariants.

4. Msg.rawjson format -- stores Anthropic content arrays
   verbatim for replay.  This is the deepest coupling: the
   stored-conversation format IS the Anthropic wire format.

5. webhttp() headers -- x-api-key + anthropic-version vs.
   Authorization: Bearer <key>.  Trivial.

6. fetchmodels() -- GET /v1/models; response shape is nearly
   identical on both (data[].id).  Trivial.

7. Error-string matching -- overlimiterr() matches "prompt is
   too long" (Anthropic); already also matches "maximum context
   length" which is the OpenAI wording.  Stop reasons differ:
   tool_use/end_turn/max_tokens vs tool_calls/stop/length.

Provider-neutral (untouched):

- Tool table (Tooldef), exectool(), runtools() bucketed
  parallelism, runcmd(), toolreplace(), toolman(), toolmk()
- Conv/Msg management, convcompact(), exchange accounting
- claudeconverse() tool-loop skeleton (send, run tools,
  append results, repeat; Maxrounds cap)
- webhttp() transport through webfs (minus header lines)
- claude9fs.c session/file-server layer (minus small knobs,
  see below)

## OpenAI Chat Completions: wire format cheat sheet

Request:
- POST /v1/chat/completions
- Authorization: Bearer <key>
- messages: [{role: system|user|assistant|tool, ...}]
  - system prompt = first message, role "system"
    (or "developer" on newer OpenAI models; start with
    "system", it is accepted everywhere)
  - assistant tool calls: {role:"assistant", content:null|text,
    tool_calls:[{id, type:"function",
                 function:{name, arguments:"<json string>"}}]}
  - tool results: ONE MESSAGE PER RESULT:
    {role:"tool", tool_call_id, content:"<result text>"}
    (vs Anthropic: all results as blocks in one user message)
- tools: [{type:"function", function:{name, description,
    parameters:<json schema>}}]  -- same schema payload as
    Anthropic's input_schema, different wrapper
- max_tokens is deprecated on OpenAI -> max_completion_tokens.
  RESOLVED: this is a per-SERVER quirk, not per-model.  All
  current models on api.openai.com accept max_completion_tokens
  (and reasoning-era models reject max_tokens outright); only
  older compatible servers know just the legacy name.  We send
  max_completion_tokens by default and flip Conv.oldmaxtok to
  fall back to max_tokens when the server complains (see
  openaiquirk and the Provider.quirk slot below).
- no cache_control: caching is automatic. Simplification.
- reasoning models: reasoning_effort: "low"|"medium"|"high"
  (maps loosely from our Thinkadaptive effort; there is no
  budget_tokens equivalent)
- stream: true, plus stream_options:{include_usage:true} to
  get usage in the final chunk

Streaming response (SSE):
- each event: data: {"choices":[{"delta":{...},
  "finish_reason":null|...}], "usage":...}
- text arrives as delta.content fragments
- tool calls arrive as delta.tool_calls[] entries with an
  "index" field; id and function.name appear once (first
  fragment), function.arguments accumulates as string
  fragments across chunks -- accumulate per index, like our
  Sblock tooljson accumulation
- reasoning deltas (some servers): delta.reasoning_content --
  display like our [thinking] markers, do NOT store/replay
- terminated by: data: [DONE]  (literal, not JSON)
- finish_reason: "stop" | "length" | "tool_calls"
  maps to: end_turn | max_tokens | tool_use

Usage object:
- prompt_tokens -> input_tokens
- completion_tokens -> output_tokens
- prompt_tokens_details.cached_tokens -> cache_read_input_tokens
- (no equivalent of cache_creation_input_tokens)

Models list:
- GET /v1/models, response {data:[{id:...}]} -- same shape.

## Design sketch

### Provider vtable (AS BUILT, step 1)

    struct Provider {            /* in claudeimpl.h, not claude.h */
        char *name;              /* "anthropic", "openai" */
        char *apiurl;
        char *modelsurl;
        int  (*headers)(int fd, char *apikey);  /* webfs ctl lines */
        Json* (*buildreq)(Conv*);               /* minus "stream" flag */
        Reply* (*readstream)(Conv*, Biobuf*, Usage*, cb, aux);
        int  (*quirk)(Conv*, char *err);        /* optional; see below */
    };

quirk (added after live testing surfaced the max_tokens
rejection): when a round fails, sendonce hands the error string
to the provider's quirk hook, which may adjust the Conv's quirk
flags and return 1 to have the request rebuilt and resent, up
to Maxquirks (3) times per round (raised from once when the
reasoning_effort ladder needed to take two steps in one
prompt).  The adjustments stick on the Conv, so later rounds
get the right shape first try.  anthropic has no quirks (nil);
openaiquirk has two cases:

- Conv.oldmaxtok toggles (max_completion_tokens vs legacy
  max_tokens) when the error names the field with
  unsupported/unknown/unrecognized phrasing -- both directions
  mention "max_completion_tokens" (OpenAI's rejection says
  "Use 'max_completion_tokens' instead"), so one rule covers
  old-server fallback and flip-back after a baseurl change.

- Conv.reasonquirk advances one rung (Reffort -> Romit ->
  Rnone -> Rdead, monotonic, never backwards) when the error
  says reasoning_effort is unsupported together with function
  tools (seen live against a "gpt-5.6-sol" model, including
  with the field never sent: the server defaults reasoning on;
  see "The reasoning_effort saga" above).  openaibuildreq
  emits the field per the current rung (effort value, omitted,
  explicit "none", suppressed) but always keeps sending tools
  -- tools are load-bearing for this program,
  reasoning_effort is a nice-to-have.

IMPORTANT (learned the hard way): the struct must stay private
to claude.c.  kencc's -T type-signature link check hashes
complete struct definitions, so an opaque "typedef struct
Provider Provider" in claude.h makes every symbol whose type
touches Provider (including Conv itself) hash differently in
claude9fs.o vs claude.o, and the link fails with "incompatible
type signatures".  Exporting the full struct would drag Reply,
ToolCall, Sblock... into the header.  So the PUBLIC handle is a
plain int index:

    Conv.prov                    int, default = anthropic (0)
    int  providerlookup(char*)   name -> index, -1 if unknown
    char* providername(int)
    char* fetchmodels(int prov, char *apikey)

claude.c resolves the index via provof(c), which clamps
out-of-range values to the anthropic entry.  sendonce() is now
a thin shell: provof -> buildreq -> +stream flag -> webhttp ->
readstream.  The "stream":true flag is added by sendonce since
both providers spell it identically; provider-specific stream
extras (OpenAI's stream_options) belong in that provider's
buildreq.

The anthropic provider entry is the old code, renamed:
buildreq -> anthropicbuildreq (tests.c updated to match),
SSE loop of sendonce -> anthropicreadstream, auth headers ->
anthropicheaders.  webhttp takes a Provider* and delegates
auth header emission to it.  claude9fs.c keeps an int defprov
(set in threadmain) for the root models file.

There is no parsemodels slot yet: both providers' models
responses are {data:[{id:...}]}, so fetchmodels stays shared
until proven otherwise.

Base-URL override matters: openai-compatible servers live at
arbitrary addresses.  Suggest a per-session "baseurl" file in
claude9fs alongside "model", plus maybe env/config default.

### The rawjson problem (the key decision)

Msg.rawjson currently stores Anthropic content arrays.  Two
options considered:

(a) Store provider-native raw messages; tag each Msg with the
    provider that wrote it.  Cheap, but a session cannot switch
    providers mid-conversation, and repair logic stays split.

(b) Define a NEUTRAL internal representation for assistant
    turns and tool results; translate to wire format in each
    provider's buildreq.  More upfront work, but:
    - sessions can switch provider/model mid-conversation
      (nice for cost: haiku vs gpt-4o-mini vs local llama)
    - repair logic operates once, on the neutral form
    - convcompact/exchange logic already provider-neutral

    Neutral form can be close to Anthropic's (it is the more
    structured of the two): a content array of
    {text | thinking | tool_use{id,name,input} |
     tool_result{id,text}} blocks.  Anthropic buildreq is then
    nearly a passthrough; OpenAI buildreq splits tool_results
    into role:"tool" messages and folds tool_use blocks into
    tool_calls.

RECOMMENDATION: (b).  Caveat: Anthropic thinking blocks carry
signatures and must be replayed verbatim within a turn, and
they are meaningless to OpenAI -- keep them in the neutral form
as opaque blocks that only the anthropic buildreq emits, and
drop them when talking to OpenAI (their absence is legal there;
OpenAI reasoning content is not replayed at all).

Migration note: existing sessions' rawjson is Anthropic-shaped;
the neutral form being a superset means old snapshots parse
as-is.  Keep striptextblocks-style tolerance when reparsing.

### Stop-reason normalization

Normalize at the readstream boundary to the Anthropic names
(claude9fs exposes stop_reason in the usage file and the
sub-agent skill documents "tool_use"/"end_turn"):
    tool_calls -> tool_use
    stop       -> end_turn
    length     -> max_tokens

### Thinking knobs

- Thinkbudget has no OpenAI equivalent; error or ignore.
- Thinkadaptive effort maps to reasoning_effort (values align:
  low/medium/high; Anthropic may accept more values -- clamp).
- claude9fs wrthinking() needs to know the provider to
  validate.

## claude9fs.c touchpoints (expected small)

- new per-session files: "provider" (anthropic|openai),
  "baseurl" (optional override)
- apikey handling: two keys (ANTHROPIC vs OPENAI env/factotum);
  key selection follows provider
- usage file: cache_creation is Anthropic-only; print 0
- wrthinking validation per provider
- graph file: unchanged (model field already free-form)

## As built (parallel sub-agent round)

File layout after the fan-out:

  claudeimpl.h  internal header: ToolCall, Tooldef, Reply,
                Provider structs; shared helper decls
                (tooldef(i), findtool, parseinput, blankstr,
                toolschema, toolfree, replyfree); openai*
                entry-point decls.  Everything fully defined
                (kencc -T; see the vtable section).
  claude.c      engine + anthropic provider (unchanged logic);
                providers[] table has both entries; sendonce
                honors Conv.baseurl over the provider default.
  openai.c      chat-completions provider: openaiheaders,
                openaibuildreq, openaireadstream, openaiquirk.
                Neutral rawjson in and out.  Output cap is
                max_completion_tokens by default (real OpenAI
                rejects max_tokens on reasoning-era models);
                Conv.oldmaxtok switches to legacy max_tokens
                for old compat servers, set automatically by
                openaiquirk when the server complains about
                the field name.  Thinkbudget silently ignored;
                Thinkadaptive -> reasoning_effort.
  claude9fs.c   new session files: provider (anthropic|openai;
                switching swaps Conv.apikey from
                $ANTHROPIC_API_KEY/$OPENAI_API_KEY, errors if
                the target key is missing) and baseurl (empty
                or "-" clears).  Both shown in ctl.
  tests.c       topenaibuildreq/topenaistream/topenaistream2:
                request shape, canned-SSE parsing incl. split
                arguments fragments, stop-reason normalization,
                usage mapping, truncated-stream failure.
                topenaiquirk/topenaiquirkreasoning: both
                openaiquirk cases (max_tokens naming toggle,
                reasoning_effort quirk ladder from a
                Thinkadaptive start).
                topenaiquirkreasoningoff: the Thinkoff case
                (field never sent; server default reasoning)
                must skip straight to explicit "none" -- see
                "The reasoning_effort saga".

Session files to use it:
  echo openai > provider        (needs $OPENAI_API_KEY)
  echo gpt-4o-mini > model
  echo 'http://myserver/v1/chat/completions' > baseurl  (optional)

## Known gaps / next steps

- No live test against openai.com itself or llama.cpp yet; the
  two live quirks found so far (max_tokens naming,
  reasoning_effort+tools) both came from a different
  openai-compatible endpoint.  The SSE parser follows the spec;
  reality may differ in small ways (e.g. servers that omit
  usage, or send roles in deltas).
- RESOLVED: request-shape quirks are reset when provider, model,
  or base URL changes, so learned state does not leak to a new
  endpoint configuration.  Whether an
  explicit "none" is accepted by OLDER compat servers that
  predate the value is untested; if one rejects it with
  matching wording the ladder falls through to Rdead and
  surfaces the error, which is the intended worst case.
- RESOLVED: the message-building loop that used to live inline
  in anthropicbuildreq (parse rawjson, strip blank text blocks,
  merge consecutive same-role messages, repair orphaned
  tool_use/tool_result) is now its own function,
  neutralmessages() in claude.c.  openaibuildreq calls it too
  and translates each repaired {role, content} entry to the
  OpenAI shape, instead of walking c->msgs directly.  A
  corrupt/interrupted history now recovers on the OpenAI path
  the same way it already did on Anthropic's.
- The root models file ignores per-session baseurl (it has no
  session); models from a local llama.cpp server are not
  listed.  Low priority: such servers usually serve exactly
  the model they were started with.
- Switching provider does not touch the model name; claudetalk
  prints a reminder after /provider, but a raw 9P client gets
  no hint.  Maybe ctl should flag a mismatch.
- Model-switch mid-conversation is supported by design (neutral
  form), but replaying anthropic thinking blocks to openai is
  handled by dropping them -- long histories with heavy
  thinking may lose context.
- RESOLVED: README documents provider setup, selection, endpoint
  overrides, session files, and claudetalk controls.

## Work plan (rough order)

1. DONE -- Provider struct (private to claude.c, int handle in
   the public API); apiurl/modelsurl/headers/buildreq/readstream
   behind it; anthropic provider = current code refactored.
   No behavior change.  claude9fs and tests both build clean;
   ./tests not yet run (needs a human to execute it).
2. DONE -- Neutral content-block form emitted and consumed by both
   provider paths.
3. DONE -- openaibuildreq translates neutral messages to Chat
   Completions JSON.
4. DONE -- openaireadstream handles text, tool calls, usage, [DONE],
   and stop-reason normalization.
5. DONE -- Shared neutral repair enforces tool-call/result ordering
   before either provider translation.
6. DONE -- provider/baseurl session files, key selection, and thinking
   mappings.
7. DONE -- per-provider model listing for configured default endpoints.
8. DONE -- request, stream, quirk, and neutral-history tests.
9. DONE -- README provider setup and controls.  Deployed skill model
   lists remain operational configuration rather than provider docs.

## Open questions

- Key storage: factotum proto=pass service=openai?  Or env
  var OPENAI_API_KEY next to the existing key handling?
  (Check how claude9fs currently obtains the Anthropic key.)
- Do any target compat servers lack streaming?  (llama.cpp
  and vllm both stream; assume stream-always like today.)
- RESOLVED: max_tokens vs max_completion_tokens is per-server,
  not per-model (no vast per-model table needed).  Sniffed from
  error responses via the Provider.quirk hook; see the vtable
  section.  reasoning_effort support remains untested against
  compat servers.
- Anthropic prompt caching saves real money and depends on
  cache_control placement; make sure the neutral-form refactor
  (step 2) preserves exactly the current placement (last tool,
  system block, last content block of final message).
