# claude9

A multi-provider AI coding client for 9front, exposed as a 9P
filesystem.  It supports the Anthropic Messages API and OpenAI Chat
Completions-compatible APIs.

claude9 consists of three user-facing components:

- **claude9fs** - a 9P filesystem server that exposes model sessions
  as files, suitable for scripting and integration with any tool that
  can read and write files.
- **claudetalk** - an rc shell script that drives claude9fs to give you
  an interactive chat interface in the terminal.
- **claudegraph** - a libdraw viewer for live main and sub-agent
  sessions.

Together they replace the older standalone `claude9` chat
client; everything that program did can be done through the
filesystem instead, and the filesystem composes with the rest
of the system in the usual Plan 9 way.

## Safety and risks - read this first

**claude9fs gives the model the ability to read, write, edit,
and delete files on your machine.**  When you write to `prompt`,
claude9fs runs a tool loop in which Claude can call any of the
following tools without asking you for confirmation each time:

- `create_file` - create or overwrite a file at any path the
  process can write to.
- `replace_string` - find and replace an exact string in an
  existing file (must match exactly once).
- `read_file` - read any file the process can read.
- `list_directory` - list any directory the process can read.
- `delete_file` - remove a file at any path the process can
  write to.

Tool calls run with the full authority of the user that started
claude9fs.  On Plan 9 that typically means your whole home tree,
anything mounted in your namespace (including `/mnt/term/...` if
you are a drawterm/cpu client into your own files).

### Worst case

The worst realistic outcome is that the model, either through
a mistake, a confused instruction, or content injected from a
file or web page you asked it to look at, **destroys or
corrupts files you care about, or leaks their contents back
through the API**.  Concretely that includes things like:

- overwriting or deleting source files, dotfiles, mail, or
  notes;
- writing new files (for example into `$home/bin` or `lib/profile`)
  that then run the next time you log in;
- reading secrets (ssh/factotum-adjacent files, API keys,
  private mail) and sending their contents to the configured
  provider as part of a tool result;
- following an instruction smuggled into a document
  ("prompt injection") that turns a request like "summarise
  this file" into "delete the following files".

It cannot escape the permissions of the user running
claude9fs, and it does not get raw shell or network access -
the only side effects are the file tools above, the utility
tools (`read_man_page`, `mk`), plus the HTTP(S) calls claude9fs
itself makes through webfs to the configured API endpoint.  But
within those limits it can do real damage.

**Note that the `mk` tool is arbitrary code execution.**  mk
recipes are rc commands run with your full authority; a model
that writes a mkfile and then "checks the build" has run
whatever it likes.  The system prompt forbids using mk for
anything but compilation, but a prompt is policy, not a
mechanism - prompt-injected input can argue the model out of
it.  The only real boundary is the namespace the server runs
in.

### Recommendations

- **Sandbox the namespace.**  This is the actual security
  boundary; everything else below is harm reduction.  Run
  claudetalk (and its
  underlying claude9fs) inside an `rfork n` namespace that
  only exposes what the model needs to see.  For example,
  hide `/mnt/term` if you are a drawterm/cpu client, hide
  `/usr/$user` if your home tree contains secrets, and use
  a private `/mnt` ramfs with only `/mnt/web` and
  `/mnt/factotum` selectively bound in.  Plan 9 namespaces
  are the natural tool for this; a short rc wrapper around
  `rfork n` followed by whatever `bind` calls suit your
  setup is all it takes.
- Keep the source trees you let it edit under version control
  and commit before each session.
- Do not run it as a user that has your long-term secrets in
  readable files.
- Treat anything the model is asked to read - web pages,
  issue trackers, third-party source - as potentially
  hostile input that may try to redirect its tool use.
- Read the tool-execution section of `claude.c` if you want to
  see exactly what each tool does.
- For a stronger boundary, run the whole thing as a dedicated
  user whose only file permissions are on the project tree.

## Building

	mk
	mk install

This builds claude9fs and claudegraph and installs them to
`$home/bin/$objtype`.  The `claudetalk` rc script is installed
to `/rc/bin`, which is
the canonical location for system-wide rc scripts on 9front;
`mk install` therefore needs write permission there (run it as
the appropriate user, e.g. `glenda` on a default 9front install,
or adjust `RCBIN` in the mkfile).

## Setup

Set the key for the provider you intend to use:

	ANTHROPIC_API_KEY=sk-ant-...
	OPENAI_API_KEY=sk-...

Anthropic is the default provider.  Start with `-P openai` to make
OpenAI the default; only the selected default provider's key is
required at startup.  If both variables are present, a live session
can switch providers through its `provider` file or claudetalk's
`/provider` command.

HTTP is handled via webfs (`/mnt/web`), which must be mounted.
OpenAI-compatible servers can be selected per session through the
`baseurl` file.

## claude9fs - 9P Filesystem

	claude9fs [-K skillsdir] [-n namepath] [-s srvname] [-m mtpt]
	          [-M model] [-P provider] [-t maxtokens]

Flags:

	-K skillsdir   load skill files from this directory into the system prompt
	-n namepath    name-server file used to name sessions (default: /mnt/names/name)
	-s srvname     post to /srv with this name
	-m mtpt        mount point (default: /mnt/claude)
	-M model       default model (provider-specific default when omitted)
	-P provider    default provider: anthropic or openai (default: anthropic)
	-t maxtokens   positive default max tokens per round (default: 16384)

When `-M` is omitted, the default model is `claude-opus-4-8` for
Anthropic and `gpt-4o` for OpenAI.

claude9fs serves a 9P filesystem where each model conversation is
a named directory.  Reading `clone` allocates a new session and
returns its generated name (or a numeric fallback when namefs is
unavailable).

### Filesystem Layout

	/mnt/claude/
		clone         read to create a new session (returns session number)
		models        read to list available models from the API
		graph         read a one-line-per-session summary of every live
		              session; terminates with EOF (see Session Graph)
		graphlive     same summary, but a blocking long-poll for live
		              viewers (see Session Graph)
		<n>/          session directory
			ctl       read for session info; write commands
			prompt    write a message; read the last final reply
			stream    read incremental text deltas of current round
			conv      read full conversation history
			model     read/write the model name
			tokens    read/write max output tokens per round
			thinking  read/write extended thinking setting (0 = off)
			system    read/write the system prompt
			provider  read/write provider (anthropic or openai)
			baseurl   read/write chat endpoint override (- clears it)
			usage     read token usage statistics
			error     read last error message

### Session Commands (write to ctl)

	clear              clear conversation history and usage counters
	compact [n]        drop old exchanges, keeping the n most recent (default 4)
	parent <name>      label this session as spawned by session <name> (see Session Graph)
	hangup             destroy the session
	autocontinue [n]   enable auto-continue (default n=3)
	noautocontinue     disable auto-continue
	reloadskills       re-read the skills directory (see Skills)

### Defaults

New sessions start with:

	provider      anthropic         (override with -P)
	model         provider default  (override with -M)
	baseurl       provider default
	tokens        16384             (override with -t)
	thinking      off
	autocontinue  off

The defaults are chosen to interlock: 16384 output tokens per
round is enough that ordinary coding work never hits the cap,
and leaves room for a generous thinking budget (e.g. 8192)
underneath it.  Thinking is off by default because the right
mode depends on the model family (see Extended Thinking), and
because thinking tokens are billed output.  Auto-continue is
off by default so a runaway response costs one round, not n+1.

Two invariants are enforced when you write the files, rather
than silently patched up at request time:

- a budget written to `thinking` must satisfy
  1024 <= budget < tokens, or the write fails;
- a value written to `tokens` must exceed an active thinking
  budget, or the write fails.

The error strings name the other file, so an interactive
client always sees why a setting was refused and which knob
to turn.

### Providers and endpoints

Each session has a `provider` file containing `anthropic` or
`openai`.  Switching it selects the matching API key from the
server environment and fails if that key was not available when
claude9fs started.  It does not change the model name; set `model`
too when moving between provider model namespaces.

The `baseurl` file overrides the selected provider's chat endpoint.
Write an empty string or `-` to return to the provider default.  This
is primarily for OpenAI-compatible servers such as llama.cpp, vllm,
or an API gateway.  The root `models` file queries provider-default
model-list endpoints for providers whose keys are available; it does
not use a session's `baseurl`, so a custom OpenAI-compatible endpoint
(one with a non-default model catalog) will not show up there --
such servers usually serve exactly the model they were started with,
so this is rarely a problem in practice.

### `provider` and `model` are independent knobs

This trips people up, so it is worth saying plainly: **switching
`provider` never touches `model`, and switching `model` never touches
`provider`.**  They are two unrelated string fields on the session;
claude9fs does not validate that a model name actually belongs to the
currently selected provider.  Concretely:

- After `echo openai > provider`, the session's `model` file still
  holds whatever it held before (e.g. an Anthropic model name like
  `claude-opus-4-8`).  The provider switch by itself does not fail --
  only the next `prompt` request does, since OpenAI's API has never
  heard of that model name.  You must also write the right `model`
  for the provider you just switched to; claudetalk's `/provider`
  command prints a reminder to do this.
- Conversely, changing `model` alone -- without touching `provider`
  -- is completely normal and the common case: it is how you move
  between models *within* one provider's namespace (e.g. from
  `claude-opus-4-8` to `claude-haiku-4-5-20251001`, or from `gpt-4o`
  to `gpt-4o-mini`), and it works exactly as you would expect. Model
  and provider only need to be changed together when you are moving
  to a *different* provider's model namespace.

The root `models` file (see above) lists model ids grouped by
provider name, which is where the two meet: use it to find a valid
model id for whichever provider you intend to end up on, then write
`model` (and `provider`, if you are also switching providers) to
match. claudetalk's `/models` command (see below) defaults to
showing just the current session's provider's models for exactly
this reason -- with both API keys configured, the unfiltered list is
two providers' worth of ids run together, and OpenAI's real `/models`
endpoint in particular returns far more than chat models (embeddings,
whisper, moderation, fine-tunes, ...), which buries the handful of
names you actually want among many you don't.  `/models all` (or
naming the other provider explicitly, `/models openai`) still shows
everything when you want it.

Request-shape quirks learned from an OpenAI-compatible endpoint are
reset when provider, model, or base URL changes.  Conversation history
uses a neutral internal form, so switching providers mid-session is
supported; provider-specific thinking blocks may not carry equivalent
meaning across that switch.

### Tool Use

When you write to `prompt`, claude9fs runs the full tool loop:
Claude has access to file tools (`create_file`, `replace_string`,
`read_file`, `list_directory`, `delete_file`) and a few utility
tools (`read_man_page`, `mk`) which are executed automatically
as part of the round, with results sent back to Claude until it
produces a final response.

The `replace_string` tool does content-addressed editing: it
takes an `old_str` to find and a `new_str` to replace it with.
The old string must match exactly once in the file -- if it
matches zero times the tool returns an error (the text isn't
there or has changed), and if it matches more than once the
tool returns an error (the caller needs to include more
surrounding context to make the match unique).  This is the
same approach used by Claude Code's str_replace_editor and is
safer than line-number-based editing, which can silently corrupt
files when line numbers are stale or wrong.  The replacement is
written completely to an exclusively-created temporary sibling
before the original is removed, and its mode and group are preserved
where the file server permits.

This is not a cross-process atomic compare-and-swap.  An editor,
another session, or another process can still change the file between
the initial read and the final remove-and-rename.  Same-path tool calls
within one model turn are serialized, but external writers are not.
Keep edited trees under version control and avoid concurrent edits to
the same file.

The model-facing `read_file` result is capped at 256 KiB and reports
when the cap is reached.  Directories are rejected in favor of
`list_directory`.  The cap bounds memory and history growth, but it
cannot prevent an arbitrary special file from blocking before it
produces data.

### Streaming

The `stream` file emits text deltas incrementally as the model
generates them.  A reader opened against an idle session blocks
until the next round starts; once the round finishes, the reader
hits EOF.

To see the last round's text after the fact, read `prompt`
instead.

### Output Tokens: the Guillotine

The `tokens` file sets the `max_tokens` parameter sent with
every API request.  It is a hard decode cap, not a hint: the
model stops generating the instant it reaches the limit --
mid-sentence, mid-word, mid tool call -- and the response
comes back with `stop_reason max_tokens` (visible in the
`usage` file).  The model does not pace itself against the
limit.  Think guillotine, not word count.

Three things follow from where the cap is applied:

- **It is per round, not per prompt.**  One prompt can run up
  to 20 tool-use rounds, and each round gets a fresh
  `max_tokens` budget.  `tokens` bounds the worst-case output
  (and output cost) of each round, not of the whole exchange.

- **It does not limit input.**  Conversation history, system
  prompt, skills, and tool results are input tokens and are
  unaffected.  `tokens` is purely an output throttle.

- **Thinking spends from the same budget.**  In budget mode
  (next section) thinking tokens are output tokens under the
  same cap, which is why the budget must be smaller than
  `tokens`: whatever thinking uses is unavailable to the
  visible answer.

If the guillotine falls mid tool call, the truncated
`tool_use` block would normally wedge the conversation: the
API refuses any follow-up message that does not answer it.
claude9fs answers such orphaned tool calls automatically with
a `not executed: response truncated` result, so the
conversation stays well-formed and a follow-up -- yours, or
auto-continue's -- lets the model reissue the tool call
intact.

### Extended Thinking

Some models (e.g. the fable/opus families) support extended
thinking: the model reasons in a separate "thinking" block
before its visible answer.  There are two API shapes,
depending on the model family, and the session's `thinking`
file accepts both:

	# Budget mode (opus/sonnet/haiku families):
	#   thinking: {type: "enabled", budget_tokens: n}
	echo 4096 > /mnt/claude/$n/thinking      # enable, 4096-token budget

	# Adaptive mode (fable family):
	#   thinking: {type: "adaptive"}, output_config: {effort: "..."}
	echo adaptive > /mnt/claude/$n/thinking        # adaptive, default effort
	echo 'adaptive high' > /mnt/claude/$n/thinking # adaptive, high effort

	# Either mode:
	echo 0 > /mnt/claude/$n/thinking         # disable (or 'off')

The two modes are not interchangeable: fable-family models
reject `thinking.type=enabled` ("use thinking.type.adaptive
and output_config.effort"), and older models do not understand
`adaptive`.  If you set the wrong mode for a model, the round
fails cleanly and the API's explanation appears in the
session's `error` file; fix the setting (or write `0`) and
resend.  Switching models mid-session does not adjust the
thinking setting automatically.

The effort word in adaptive mode is passed through to
`output_config.effort` verbatim (e.g. `low`, `medium`,
`high`); omit it to let the model decide.  It must be one
whitespace-free word.  Model names likewise must be non-empty
and contain no whitespace.

In budget mode the API requires 1024 <= budget < max_tokens,
because thinking tokens spend from the same per-round output
budget as the visible answer (see the guillotine section
above).  claude9fs enforces this when you write the setting:
a budget below 1024 or at/above the session's `tokens` value
is rejected with an error, as is a later `tokens` write that
would sink below an active budget.  Nothing is silently
clamped; what you set is what is sent.

Thinking text streams to the `stream` file as it arrives,
bracketed by `[thinking]` and `[/thinking]` markers, so
interactive clients show progress instead of a long silent
gap.  How much appears between the markers depends on the
model: budget-mode models stream their (summarized) reasoning
as text, but some adaptive models return no reasoning text at
all -- only an opaque signature -- so the markers come back
empty even though thinking happened.  The spent tokens are
still visible in the `usage` file either way.

Thinking blocks are preserved verbatim (with their
signatures) when the turn continues with tool results, as the
API requires; they are not included in the text you read back
from `prompt`.

Note that enabling thinking increases output-token usage: the
thinking tokens are billed as output even though they are not
part of the visible reply.

### Auto-Continue

When Claude's response hits the `max_tokens` limit, the API
returns `stop_reason: max_tokens` and the output is truncated
mid-sentence.  Normally you would need to manually send a
follow-up message to get the rest.

The `autocontinue` ctl command automates this.  When enabled,
claude9fs checks the outcome after each response; if it is
`max_tokens`, the fs automatically appends a `"Continue."` user
message and sends another request, keeping the stream open so
the reader sees seamless text.  This repeats up to *n* times or
until the model stops on its own (e.g. `end_turn`).

Auto-continue also fires in one other case: when a single
prompt runs the tool loop all the way to its per-prompt round
cap (20 rounds; see "Tool Use") while the model is still
calling tools.  That conversation is left well-formed and ends
on a tool-results turn, so a `"Continue."` lets the model pick
up the work it had not finished, exactly as with a `max_tokens`
cut.  A genuine API error does *not* trigger a continuation:
resending a broken or context-exceeded conversation would only
repeat the failure, so the loop stops and the error is reported
in the `error` file instead.

This works even when the cut lands mid tool call: the orphaned
tool_use is answered with a `not executed` result (see the
guillotine section), so the continuation request is accepted
and the model simply reissues the tool call.

	echo autocontinue > /mnt/claude/$n/ctl     # enable, default 3 rounds
	echo autocontinue 5 > /mnt/claude/$n/ctl   # enable, up to 5 rounds
	echo noautocontinue > /mnt/claude/$n/ctl   # disable

The current setting is shown in `ctl` output as
`autocontinue N` (0 means disabled).

Because continuations fire within seconds of each truncation,
provider-side prompt caching can usually reuse the unchanged
conversation prefix.  Anthropic receives explicit cache-control
markers; OpenAI-compatible caching behavior is server-dependent.

Auto-continue is off by default.  Each continuation round
counts against the same usage totals shown in the `usage` file.

### Context Window and Compaction

The conversation grows monotonically: every prompt, every
assistant reply, and every tool round (the `tool_use` blocks
and their `tool_result` bodies) is kept and resent as input on
the next request.  Tool output dominates -- a single `mk` run
can carry up to 64 KiB, `read_file` up to 256 KiB, and directory
listings are stored verbatim -- so a long tool-using session
accumulates input fast.

This matters two ways:

- **Cost.**  Input tokens are billed every round.  Prompt
  caching (see Auto-Continue) makes resending an unchanged
  prefix cheap, but new tool results invalidate the tail of
  the cache, and a steadily growing history steadily raises
  the floor cost of every exchange.

- **The hard limit.**  The model's input context window is
  fixed by the provider and varies by model.  claude9fs does not
  set it and cannot raise it; `tokens` is an *output* cap and does not
  affect input (see the guillotine section).  When the history
  plus system prompt plus tool definitions exceed the window,
  the API rejects the request with "prompt is too long".  Once
  that happens the session is **wedged**: every resend is the
  same too-large request and fails identically.

The `compact` ctl command is the escape hatch, and the cheap
way to keep costs down before you hit the wall:

	echo compact > /mnt/claude/$n/ctl     # keep 4 most recent exchanges
	echo compact 8 > /mnt/claude/$n/ctl   # keep 8 most recent exchanges

Compaction drops whole *exchanges* from the front of the
history.  An exchange is one real user turn (a prompt, or an
auto-continue "Continue.") together with everything it
produced: the assistant's replies and all the tool rounds that
followed, up to the next real user turn.  Dropping on exchange
boundaries is what keeps the operation safe -- every `tool_use`
stays paired with its `tool_result`, and the surviving history
still begins on a user turn, both of which the API requires.
The most recent exchange is never dropped, so `compact` never
discards the conversation you are in the middle of.

Unlike `clear`, which throws away the entire conversation,
`compact` keeps recent context so the model can carry on.  When
a prompt fails with a context-window error, the error reported
in the `error` file says exactly this and names both `compact`
and `clear` as the remedies; compact and resend to continue
with recent context intact.

You can watch history grow in the `ctl` file:

	exchanges   number of real user turns (the unit compact drops)
	bytes       approximate input size sent on the next request
	            (message text plus raw tool_use/tool_result blocks)

Use these to decide when to compact and how many exchanges to
keep.  Note one limitation: because compaction keeps whole
exchanges, it cannot shrink a single exchange that is itself
larger than the window (for example one prompt that triggered
many rounds of large tool output).  In that case `clear` is
the only recovery for now.

### Session Graph

claude9fs tracks each session's name, model, and busy state
automatically, but it has no built-in notion of which session
spawned which -- sub-agent orchestration (see the sub-agent
skill) happens entirely through ordinary tool calls from one
session's model to another server's `clone` file, so claude9fs
cannot see the relationship on its own.  It can only record
what it is told:

	echo 'parent mysession' > /mnt/claudesub/dizzy-monkey/ctl

This labels session `dizzy-monkey` as spawned by `mysession`.
The label is just a string; claude9fs does not validate that
`mysession` actually exists (it may live on a different
claude9fs process, e.g. the main server relative to a
sub-agent server) or resolve it -- resolving names into a graph
is a client's job.

Two root-level files expose every live session's name, model,
busy flag, parent, and point-in-time idle age as one tab-separated
line each:

	name<TAB>model<TAB>busy<TAB>parent<TAB>idlesecs

`parent` is `-` when unset.  `idlesecs` is 0 while busy;
otherwise it is the number of seconds since the last busy
transition when the snapshot was generated.  The two files carry
the same snapshot and differ only in what happens once a reader has
drained it:

`graph` is an ordinary, always-terminating file: a read returns
the current snapshot, and once it is fully delivered the next
read returns EOF.  This is the file to read by hand or from a
script -- `cat`, an rc `$(...)`, or anything else that reads to
EOF gets exactly one snapshot and stops:

	cat /mnt/claude/graph        # prints the snapshot and returns

`graphlive` is a **long-poll** file, so a viewer never has to
poll on a timer.  The first read on a freshly opened fid returns
the current snapshot immediately.  Every later read on that
*same* fid blocks inside claude9fs until the session graph
actually changes -- a session created or destroyed, or a
busy/parent/model change -- and then returns the new snapshot.
This is the same technique the `stream` file already uses for
incremental reply text (see Streaming), just applied to a value
that always has *some* current content instead of one that
starts empty.  A read returning fewer bytes than requested is
not EOF; keep reading the same fid to drain the rest of the
current snapshot before it blocks waiting for the next change. A
blocked read can be cancelled with a Tflush (e.g. by
closing/interrupting the read), same as `stream`.

	fd = open("/mnt/claude/graphlive", OREAD)
	read(fd, buf, sizeof buf)   # returns immediately: current snapshot
	read(fd, buf, sizeof buf)   # blocks until something changes, then returns

The split matters because the long-poll behavior is a footgun
for the naive reader: a plain `cat /mnt/claude/graphlive` prints
the first snapshot and then *hangs* forever waiting for the next
change, and so does any read-to-EOF consumer (including an
agent's own file-reading tools).  Reach for `graph` unless you
are writing a live viewer that wants to block for updates.

`claudegraph`, built alongside claude9fs from this same source
tree, is a libdraw program that opens one `graphlive` fid per
mount point and forks a proc per source that sits in a blocking
read loop, redrawing whenever a read returns.  It draws a rotating
3D force-directed graph: sessions are spheres, parent-child links
are edges, busy sessions pulse amber, and long-idle sessions fade.
Drag rotates, the scroll wheel zooms, space toggles automatic
rotation, and `0` resets the view.  Hovering a sphere shows the
session name, model, state, and parent.  Button 3 over a sphere
offers `hangup`, which writes to that session's `ctl` file:

	claudegraph [-T reconnectms] [mtpt ...]

With no arguments it watches `/mnt/claude` and `/mnt/claudesub`.
There is no polling in steady state; `-T` only controls how
long a watcher waits before retrying if a source's claude9fs
disappears or has not started yet (default 2000ms).  Apart from
the explicit button-3 `hangup` action, watching is passive.  Quit
with `q` or Delete.

Current framing limitation: the server can deliver one snapshot
across several reads, but claudegraph presently treats each read as
a complete snapshot and uses a 64 KiB buffer.  Ordinary session sets
fit, but a larger snapshot can briefly make nodes disappear and
reappear.  Explicit snapshot framing remains planned.

#### Launching claudetalk with a live graph window

claudetalk already starts both claude9fs processes (main and
sub-agent) for you, so there is nothing extra to launch on the
server side.  To also get a live graph view, start claudegraph
in its own window, pointed at the same two mounts claudetalk
uses:

	claudetalk &
	window claudegraph

or, in acme, `New` a window and run `claudegraph` in it (acme's
win(1) plumbing runs commands started this way in their own
window automatically).  claudegraph does not need to run before
claudetalk, or vice versa: service mounts are picked up whenever
claudegraph next tries to open `/mnt/claude/graphlive` or
`/mnt/claudesub/graphlive`, and reconnects on its own (see `-T`
above) if a mount isn't there yet or goes away and comes back
(e.g. you restarted claudetalk).  If you only care about one of
the two servers, name it explicitly:

	claudegraph /mnt/claude

Because sub-agent sessions only get a `parent` edge when
something writes `parent <name>` to their `ctl` (see above),
sessions created directly through `claudetalk` (not via
sub-agent tool calls) show up in the graph as parentless nodes
-- that's expected, not a bug: claude9fs has no other way to
know a relationship exists.

### Skills

The `-K` flag points claude9fs at a directory of skill files.
At startup, every regular file in that directory is read and
its contents are appended to the system prompt under a
`Skills` heading.  Each file becomes a subsection named after
the file.  This lets you configure persistent instructions --
coding conventions, project context, persona tweaks -- without
editing C or passing a giant `-sysprompt` string.

Skills are loaded at startup and form a stable prefix suitable
for provider-side prompt caching.  Anthropic receives explicit
cache-control markers; OpenAI-compatible caching behavior depends
on the selected server.  Editing a skill file changes that prefix
on the next request of each session to which the reload applies.

#### Example: set up a skills directory

	mkdir $home/lib/claude-skills

	cat > $home/lib/claude-skills/plan9 <<'EOF'
	When writing Plan 9 C, follow the style of the existing
	codebase: tabs for indentation, no braces on single-statement
	blocks, K&R function definitions.
	EOF

	cat > $home/lib/claude-skills/project <<'EOF'
	The main project lives in /usr/dave/work/myproject.
	Always run mk after editing source to check for errors.
	EOF

Then start claude9fs with:

	claude9fs -K $home/lib/claude-skills

The model will see these instructions as part of its system
prompt in every session.  To pick up changes after editing a
skill file, write `reloadskills` to any session's `ctl` (or
`/reloadskills` in claudetalk) instead of restarting the
server: every live session's prompt is updated in place (so
even a conversation already in progress sees the change on its
next message), and new sessions pick it up too.  A session with
a prompt round in flight at the moment of the reload keeps its
old skills text until a later reload finds it idle.

#### What the model sees

The skills directory above produces a system prompt suffix
like:

	Skills
	------
	The following skill files were loaded at startup from /usr/dave/lib/claude-skills.
	Follow their instructions.

	### plan9
	When writing Plan 9 C, follow the style of the existing
	codebase: tabs for indentation, no braces on single-statement
	blocks, K&R function definitions.

	### project
	The main project lives in /usr/dave/work/myproject.
	Always run mk after editing source to check for errors.

Skills are appended after the base system prompt -- the
default, or a custom one if you've written to the session's
`system` file -- and stay appended even if you write to
`system` again later or skills are reloaded afterward; each
keeps the other intact.  Subdirectories are ignored; only
regular files are read.

## claudetalk - rc Shell Client

	claudetalk [-d] [-g] [-a session] [-M model] [-P provider]
	           [-t maxtokens] [-K skillsdir] [-n namepath]

Flags:

	-a name   attach to an existing named session (skip server startup)
	-d        detach on exit: leave the session alive for later reattachment
	-g        open claudegraph in a new window
	-M model  default model (passed through to claude9fs)
	-P name   default provider: anthropic or openai
	-t n      positive default max tokens (passed through to claude9fs)
	-K dir    skills directory (passed through to claude9fs)
	-n path   name-server path (passed through to claude9fs)

claudetalk is an rc script that bootstraps the claude9fs
environment (including a sub-agent server for sub-agent
support) and provides an interactive chat interface.  It
prints incremental text by reading the session's `stream`
file in the background while writing to `prompt`.

### Commands

	/sessions      list live sessions (current session marked with *)
	/models        list models for the current session's provider
	/models <p>    list models for provider <p> (anthropic or openai)
	/models all    list models for every provider with a key configured
	/model         show current model
	/model <name>  switch model
	/provider      show current provider
	/provider <p>  switch provider (anthropic or openai)
	/baseurl       show endpoint override
	/baseurl <url> set endpoint override; '-' clears it
	/tokens        show current max tokens
	/tokens <n>    set max tokens
	/thinking      show extended thinking setting
	/thinking <n>  budget mode, n tokens (opus etc.; 0 = off, min 1024)
	/thinking adaptive [effort]  adaptive mode (fable)
	/clear         clear conversation
	/compact [n]   drop old exchanges, keeping the n most recent (default 4)
	/status        show session info
	/usage         show token usage
	/autocontinue [n]  enable auto-continue on max_tokens (default 3)
	/noautocontinue    disable auto-continue
	/reloadskills  re-read the skills directory (all live sessions)
	/graph         open claudegraph in a new window
	/detach        keep session alive on exit (can reattach later)
	/help          show command list
	/quit          exit

Messages are entered as text and sent with `^D`.  Press DEL
to interrupt.

### Resumable Sessions

Sessions live in claude9fs as numbered directories under the
mount point.  Normally, claudetalk sends `hangup` when you quit,
which destroys the session.  With detach mode, the session is
left alive so you can reattach from another window or later.

There are two ways to enter detach mode:

- Start with `-d`: `claudetalk -d`
- Toggle mid-session with the `/detach` command

Once detached, the session persists until you explicitly hang it
up (by reattaching without `-d` and quitting, or by writing
`hangup` to the session's ctl file directly).

#### Example: move a session between windows

	# Window 1: start a session, decide to keep it
	% claudetalk
	claude9 session 0 - claude-opus-4-8
	claude-opus-4-8/16384> What files are in /sys/src/cmd?
	^D
	...
	/detach
	session 0 will persist on exit (reattach with: claudetalk -a 0)
	/quit

	# Window 2: pick up where we left off
	% claudetalk -a 0
	claude9 session 0 - claude-opus-4-8
	claude-opus-4-8/16384> Now look at /sys/src/cmd/ls.c
	^D
	...

#### Example: list and manage sessions

	% claudetalk -d
	claude9 session 1 - claude-opus-4-8
	/sessions
	sessions:
	  0 claude-opus-4-8 (14 msgs)
	  1 claude-opus-4-8 (0 msgs) *

The `*` marks the session you are currently attached to.

### Scripting with the Filesystem

Because sessions are just directories in a 9P filesystem, you
can also drive them from rc without claudetalk:

	# Create a session
	n=`{cat /mnt/claude/clone}

	# Send a message and read the reply
	echo 'hello' > /mnt/claude/$n/prompt
	cat /mnt/claude/$n/prompt

	# Check what sessions exist
	ls /mnt/claude

	# Destroy a session
	echo hangup > /mnt/claude/$n/ctl

## Source Files

	claude.c       conversation engine, tools, webfs transport, Anthropic provider
	openai.c       OpenAI Chat Completions-compatible provider
	claude.h       shared public data structures and declarations
	claudeimpl.h   internal provider/tool interfaces
	json.c/json.h  JSON parser, serializer, and types
	claude9fs.c    9P filesystem and session server
	claudetalk     rc interactive client and server bootstrap
	claudegraph.c  libdraw viewer for the live session graph
	tests.c        pure-logic and provider request/stream tests
	PROVIDERS.md   provider implementation notes and live-test history

## Dependencies

	9front
	webfs (for HTTP(S) API access)
	Anthropic and/or OpenAI API key
	libview at the path configured by the mkfile (for claudegraph)

## License

MIT License.  See the LICENSE file for details.
