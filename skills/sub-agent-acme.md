# Skill: Coordinating a Sub-Agent via claude9fs and Acme

## What this does

Spawn a sub-agent as a session on the sub-agent claude9fs
server, send it a task, and deposit the result in an acme
window -- all using ordinary file reads and writes from the
outer agent's tool loop.

Acme is optional.  If it isn't mounted, or you don't need a
persistent window, just report results directly in your own
reply instead of depositing into a window -- everything else
below still applies.

## Setup

claudetalk automatically starts two claude9fs processes:

1. A **sub-agent server** at `/mnt/claudesub` (srv name `claudesub`)
2. The **main server** at `/mnt/claude` (srv name `claude`)

The sub-agent server starts first so the main server inherits
it in its namespace.  This means tool calls from the outer
agent can reach `/mnt/claudesub` without any manual setup.

Just run `claudetalk` and sub-agent support is available.

## Why two processes are needed

A single claude9fs process cannot serve sub-agents to itself.
The reason is a 9P deadlock: when the outer agent (which is
being served by claude9fs) issues a `read_file` on `clone`,
that read becomes a 9P Tread on the same server.  But the
server thread is blocked waiting for the outer agent's tool
call to complete.  Neither side can make progress.

**Symptoms:** reading `/mnt/claude/clone` returns "i/o on
hungup channel" or hangs indefinitely.

The second process on a separate pipe eliminates this entirely.

## Why namespace order matters

On Plan 9, each process has its own namespace -- a private
mapping of paths to file trees.  Namespaces are inherited
from parent to child at fork time.  They are NOT shared
retroactively.

The sub-agent server must be started BEFORE the main server
so the main server's namespace includes `/mnt/claudesub`.
claudetalk handles this automatically.

Tool calls that the agent makes (read_file, create_file,
etc.) are executed by the main claude9fs process, not by
claudetalk.  So it is the main claude9fs's namespace that
matters, not the shell's.

## Step by step

### 1. Create a new acme window

Write to `/mnt/acme/new/body` to create a window with initial
content, or open `/mnt/acme/new/ctl` and write a `name` command
to create an empty named window.

    create_file /mnt/acme/new/ctl "name /tmp/my-output\n"

Then read `/mnt/acme/index` to find the window ID.  The index
has fixed-width fields: the first 11 characters are the window
ID, and the name starts at character position 60.

**Gotcha:** opening any file under `new/` allocates a window as
a side effect.  Writing `name` to `new/ctl` can create a stray
unnamed window alongside the named one.  Check the index and
delete strays with `create_file /mnt/acme/<id>/ctl "delete\n"`.

### 2. Create a sub-agent session

Read `clone` on the **sub-agent** claude9fs mount:

    read_file /mnt/claudesub/clone   -->  "dizzy-monkey"

This allocates a new independent session on the second server.
Do NOT read `/mnt/claude/clone` -- that will deadlock.

**Tag it with its parent, for the graph view.**  claude9fs has
no way to know that you (some session) just spawned this one --
it only knows what you tell it.  Right after cloning, write your
own session's name to the new session's `ctl` as `parent`:

    create_file /mnt/claudesub/dizzy-monkey/ctl "parent my-session-name"

If a live viewer (`claudegraph`, see below) or a script is
watching, this is the only thing that makes the parent/child
edge show up.  It costs one write and is easy to forget, so make
it part of the same step as cloning, not an afterthought.  If
you don't know your own session's name, it's whatever name your
own claude9fs session directory has (visible in the `ctl` file
of the session serving *you*, or in the greeting most clients
print at startup).

### 3. Configure the session

Set the model to something cheap for simple tasks:

    create_file /mnt/claudesub/dizzy-monkey/model "claude-haiku-4-5-20251001"

Optionally set a system prompt to keep the sub-agent focused:

    create_file /mnt/claudesub/dizzy-monkey/system "You are a concise summarizer."

### 4. Send a prompt and read the reply

Write the user message to `prompt`.  The write blocks until
the model finishes (including any tool-use loops):

    create_file /mnt/claudesub/dizzy-monkey/prompt "Summarize X."

Read `prompt` back to get the last assistant reply:

    read_file /mnt/claudesub/dizzy-monkey/prompt   -->  (the summary)

**IMPORTANT: sub-agents have tools and share your namespace.**
The sub-agent claude9fs process has the same tools you do
(read_file, create_file, list_directory, mk, etc.) and
shares the same filesystem namespace.  This means sub-agents
can read files, search code, and examine source directly.

**Do NOT read file contents yourself just to paste them into
the sub-agent's prompt.**  That wastes tokens twice -- once
for you to read, once to send.  Instead, tell the sub-agent
which files to read:

    WRONG (wasteful):
      contents = read_file /sys/src/9/vz64/audiovz.c
      create_file .../prompt "Review this code:\n<contents>"

    RIGHT (efficient):
      create_file .../prompt "Review /sys/src/9/vz64/audiovz.c.
        Read it with read_file, then give me your analysis."

The sub-agent will use its own tool calls to read the file.
This is especially important for large files -- you avoid
doubling the token cost.  Be assertive in telling the
sub-agent to use its tools; some models (especially Haiku)
may hesitate or claim they cannot read files, but they can.
Phrasing like "Use read_file to read X" or "You have tools;
read the file directly" resolves any confusion.

### 5. Deposit the result in the acme window

Write the text to `/mnt/acme/<id>/body`.  Writes to `body`
are always appended (file offset is ignored):

    create_file /mnt/acme/14/body "<the result text>"

The text appears immediately in the acme window.

### 6. Clean up

Hang up the sub-agent session:

    create_file /mnt/claudesub/dizzy-monkey/ctl "hangup"

## Running several sub-agents in parallel

Multiple sub-agent sessions genuinely run concurrently: the
claude9fs process serving them executes a turn's tool calls in
parallel (one Plan 9 process per call) whenever a single model
turn issues more than one tool_use block.  In practice this
means: clone several sessions, configure each, and issue all
their `prompt` writes as independent tool calls in the *same*
reply -- not one at a time, waiting for each to return before
starting the next -- and they will actually run at the same
time instead of queuing up.

    read_file /mnt/claudesub/clone   -->  "0"
    read_file /mnt/claudesub/clone   -->  "1"
    read_file /mnt/claudesub/clone   -->  "2"
    (configure each: model, system)
    (issue all three prompt writes together, in one reply)

If sub-agent work still seems to serialize even when you batch
the calls this way, that is a bug worth noticing and reporting,
not the expected behavior -- see claude.c's `runtools` for how
concurrent tool execution is actually implemented.

### Partition file ownership to avoid collisions

Sub-agents share your namespace and can write anywhere you can.
If two sessions edit the same file at the same time, you get a
race, not a merge.  Before spawning, split the work so each
session's file edits are disjoint:

- Give each sub-agent its own new file(s)/directory to create,
  or a clearly separate region of existing source to touch.
- Reserve shared integration surfaces -- top-level build files
  (mkfile), project status/plan docs (README, STATUS, TYPES,
  DESIGN, ...), anything all sessions would otherwise want to
  "register" a new thing into -- for yourself, the coordinator,
  to edit once *after* reviewing everyone's output.  Tell each
  sub-agent explicitly not to touch those files.
- If a project builds or tests against an *installed* copy of
  an artifact (a compiled binary found via $PATH, a library
  file copied into a lib directory) rather than the working-
  tree copy, say so up front.  Otherwise a sub-agent burns
  tool-call rounds rediscovering that its source edits aren't
  taking effect before figuring out which copy actually
  matters -- this happened in practice and cost a real chunk
  of one sub-agent's round budget.
- Acme is unnecessary for this kind of fan-out; just collect
  each session's result and report a consolidated summary
  yourself once everyone is done.

## The tool-loop round cap and autocontinue

Each `prompt` write runs up to 20 tool-use rounds internally
(the hard-coded `Maxrounds` in claude.c) before claude9fs cuts
the exchange off, even if the model still wants to call more
tools.  A sub-agent working through a multi-file task (read
several files, make several edits, run mk, fix, rerun) can
easily hit this in a single prompt, especially the first
message, which also has to spend rounds reading context before
it can act.

**How to tell this happened:** the conversation is left
well-formed (every tool_use has a matching tool_result), so it
looks like a normal reply -- but it stops mid-task, often
mid-sentence, with no closing summary even if you asked for
one.  Do not rely on the reply text alone to decide whether a
sub-agent finished; check `usage`'s `stop_reason` after a
prompt write returns:

    read_file /mnt/claudesub/0/usage
    -->  stop_reason tool_use     (cut off by the round cap)
    -->  stop_reason end_turn     (the model actually stopped on its own)

`error` reports the same condition explicitly, as
"tool loop limit reached (20 rounds)".

**Recommended: turn on autocontinue before sending the task**,
so claude9fs resumes automatically instead of you having to
poll and manually re-prompt:

    create_file /mnt/claudesub/0/ctl "autocontinue 3"

This also covers the sibling `max_tokens` case (a single
round's output hitting its own token cap), which looks similar
and is handled the same way.  Autocontinue sends a plain
"Continue." on your behalf, up to the given number of times,
and stops early if the model reaches a natural end (`end_turn`).

**If you didn't set autocontinue and a task gets cut off**,
resume it yourself with a *specific* continuation prompt --
name exactly where the transcript left off (what it had just
read, decided, or was mid-edit on) rather than a bare
"continue", so the sub-agent doesn't spend its next round
budget re-reading files or re-deriving a plan it already had:

    create_file /mnt/claudesub/0/prompt "Continue exactly where
      you left off -- you had just finished X and were about to
      do Y. Don't re-read files you've already read. If you
      run out of rounds again, stop at a safe, consistent point
      and summarize what's left."

**Verify independently regardless.**  Even a session that
*does* produce a tidy final summary can be wrong or optimistic
about what it actually finished.  And a session that got cut
off mid-transcript, with no final summary at all, may still
have already completed real, correct work in its file edits
before running out of rounds -- absence of a summary is not
evidence of absence of progress.  Either way, check the actual
filesystem state yourself: read the files it claims to have
changed, and run the project's own build/test via `mk`, before
reporting results onward.  This is the only way to actually
know, as opposed to trusting a transcript.

## Notes

- **Sub-agents share the namespace and have tools.**  Every
  sub-agent session has the same read_file, create_file,
  list_directory, mk, and read_man_page tools as the outer
  agent, operating on the same filesystem namespace.  They
  can read any file you can read.  Always tell sub-agents
  to read files themselves rather than pasting file contents
  into their prompts.  This saves tokens and context space.
  If csearchfs is mounted at /n/csearch, sub-agents can use
  that too.

- **Two servers, many sub-agents:** The sub-agent server at
  `/mnt/claudesub` can host many sessions via `clone`.  You
  only need the one extra process, not one per sub-agent.

- **Model per session:** Each session has its own `model` file.
  The outer agent can run on Opus while sub-agents run on Haiku
  to save costs.

- **UTF-8 artifacts:** Text round-tripping through the API can
  mangle multi-byte characters (em-dashes, curly quotes).
  Clean up or use ASCII when depositing into acme.

- **Streaming:** For long sub-agent outputs, you could read
  `stream` instead of `prompt` for incremental deltas, but
  `prompt` is simpler for batch use.  Note that re-reading
  `prompt` without writing a new one does NOT show live
  progress -- it just returns the same last-final-reply text
  every time until you send another prompt (or a
  continuation).  A `create_file` write to `prompt` blocks
  until that whole exchange finishes, including its internal
  tool rounds (up to the round cap described below), so by the
  time the write returns, that session's turn is genuinely
  over, not still running in the background.

- **Detach mode:** If claudetalk is run with `-d`, the servers
  stay alive on exit.  Reattach with `claudetalk -a <session>`.
  The sub-agent server persists too, accessible at
  `/mnt/claudesub` or via `/srv/claudesub`.

- **Already running:** If `/srv/claude` or `/srv/claudesub`
  already exist (e.g. from a detached session), claudetalk
  reuses them instead of starting new processes.

## Watching the graph live: claudegraph

If you tag sessions with `parent` as described in step 2, the
`graph` file at the root of each claude9fs mount (e.g.
`/mnt/claude/graph`, `/mnt/claudesub/graph`) lists every live
session as one tab-separated line:
`name  model  busy  parent  idlesecs`
(idlesecs is 0 while busy, else seconds since the last prompt
round started or ended, as of the moment of the read).  This is
machine-readable and meant for tools, not for you to narrate
back to the user verbatim.

`claudegraph` (built alongside `claude9fs` from this same
source tree) is a libdraw program that watches both mounts and
draws the sessions as a rotating 3D force-directed graph
(ubigraph-style): spheres connected by parent->child edges,
busy sessions pulsing amber, long-idle sessions fading toward
the background.  Drag rotates, scroll zooms, space toggles the
slow auto-rotation, hovering a node shows its name/model/state,
and button 3 over a node offers a `hangup` menu entry -- handy
for cleaning up sub-agents someone forgot to hang up, since
sessions live until an explicit hangup.  It does not poll: the
`graph` file is a long-poll file (a blocking read that returns
immediately with the current snapshot, then blocks until the
next change), the same technique the `stream` file already
uses for reply text, so updates appear the instant something
changes rather than up to a second late.  If the user wants a
live visual of sub-agent activity, tell them to run it in a
new window rather than trying to reconstruct the graph yourself
from repeated `ls`/`ctl` reads:

    window claudegraph

or, if they already have specific mounts:

    claudegraph /mnt/claude /mnt/claudesub

It is read-only against the filesystem (just reads `graph`
files, blocking between changes rather than polling) and has
no effect on sessions; it is safe to leave running for the
whole life of a claudetalk session.

## Models (as of this writing)

    claude-haiku-4-5-20251001      cheapest, good for simple tasks
    claude-sonnet-4-6              mid-range
    claude-opus-4-8                most capable

## Security note: mk is not a shell

The `mk` tool exists only for checking whether code compiles.
Never use it to execute arbitrary commands, run scripts, or
achieve side effects beyond compilation.  Do not create or
modify mkfiles to smuggle shell commands through mk.  This
rule is also enforced in the system prompt (see `convnew()`
in `claude.c`).  If you need to run a command, ask the user
to do it -- the tool set intentionally has no `exec` or
`run_command` capability.

## Maintaining this skill file

This file is loaded from a *deployed* skills directory (e.g.
`/usr/dave/lib/claude9/skills`), but the real source tree for
claude9 -- `claude.c`, `claude9fs.c`, and this `skills/`
directory itself -- lives at `/usr/dave/work/claude9`.  When
updating this skill based on something learned about claude9's
actual behavior (round caps, autocontinue, streaming, tool
concurrency, etc.), read the real source there first rather
than guessing from this document or from what a deployed copy
happens to say -- `claude.c` has the tool loop (`claudeconverse`,
`Maxrounds`, `toollimiterr`), autocontinue, and streaming logic
all in one file.  Edit the copy under
`/usr/dave/work/claude9/skills/`, not just a deployed one, so
the change survives a redeploy; then propagate it to whatever
deployed skills directory is actually in use (check the skill
list in your own system prompt for the path it was loaded
from).
