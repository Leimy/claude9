# Skill: Coordinating a Sub-Agent via claude9fs and Acme

## What this does

Spawn a sub-agent as a session on the sub-agent claude9fs
server, send it a task, and deposit the result in an acme
window -- all using ordinary file reads and writes from the
outer agent's tool loop.

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
  `prompt` is simpler for batch use.

- **Detach mode:** If claudetalk is run with `-d`, the servers
  stay alive on exit.  Reattach with `claudetalk -a <session>`.
  The sub-agent server persists too, accessible at
  `/mnt/claudesub` or via `/srv/claudesub`.

- **Already running:** If `/srv/claude` or `/srv/claudesub`
  already exist (e.g. from a detached session), claudetalk
  reuses them instead of starting new processes.

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
