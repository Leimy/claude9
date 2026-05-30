# Skill: Coordinating a Sub-Agent via claude9fs and Acme

## What this does

Spawn a sub-agent as a second claude9fs session, send it a
task, and deposit the result in an acme window -- all using
ordinary file reads and writes from the outer agent's tool
loop.

## Critical: you need a SECOND claude9fs process

A single claude9fs process cannot serve sub-agents to itself.
The reason is a 9P deadlock: when the outer agent (which is
being served by claude9fs) issues a `read_file` on `clone`,
that read becomes a 9P Tread on the same server.  But the
server thread is blocked waiting for the outer agent's tool
call to complete.  Neither side can make progress.

**Symptoms:** reading `/mnt/claude/clone` returns "i/o on
hungup channel" or hangs indefinitely.

**Solution:** the user must start a second claude9fs process
and mount it at a separate path (e.g. `/mnt/claudesub`):

    % claude9fs -s claudesub -m /mnt/claudesub

## Critical: startup order matters (namespace inheritance)

The outer claude9fs (the one serving the agent's session)
executes the agent's tool calls -- `read_file`, `create_file`,
etc. -- inside **its own process namespace**.  Plan 9
namespaces are per-process and inherited at fork time.  This
means:

- If you start the sub-agent claude9fs **after** the outer
  claude9fs, the outer process never sees `/mnt/claudesub`.
  It was not in the namespace when the outer process started,
  and mounting it later in your shell only affects your
  shell's namespace, not the already-running claude9fs.

- `claudetalk` (the rc script) runs in your shell and CAN
  see both mounts.  But claudetalk is just a thin wrapper --
  the actual tool execution happens inside the outer
  claude9fs process, which has its own isolated namespace.

**The sub-agent server MUST be started BEFORE the outer
agent server**, so the outer server inherits the mount:

    % acme                                     # acme first
    % claude9fs -s claudesub -m /mnt/claudesub # sub-agent server
    % claude9fs -s claude                      # outer server (inherits /mnt/claudesub)
    % claudetalk                               # start the chat

If you get the order wrong, the agent will see `/mnt/claudesub`
as empty or nonexistent even though your shell can see it fine.

**Debugging clue:** if `list_directory /mnt/claudesub` returns
empty (no `clone` file) but the mount exists, the outer
claude9fs was started before the sub-agent server.  Restart
in the correct order.

The outer agent now has (in its namespace):
- `/mnt/claude` -- its own session (do NOT clone from here)
- `/mnt/claudesub` -- the sub-agent server (clone from here)
- `/mnt/acme` -- acme's filesystem

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

### 5. Deposit the result in the acme window

Write the text to `/mnt/acme/<id>/body`.  Writes to `body`
are always appended (file offset is ignored):

    create_file /mnt/acme/14/body "<the result text>"

The text appears immediately in the acme window.

### 6. Clean up

Hang up the sub-agent session:

    create_file /mnt/claudesub/dizzy-monkey/ctl "hangup"

## Why the deadlock happens (details)

claude9fs is a 9P file server built on lib9p.  When the outer
agent calls `read_file /mnt/claude/clone`, the tool
implementation opens and reads a file.  That file lives on the
same 9P server.  The lib9p event loop dispatches the Tread to
`fsread`, which calls `newsession()` and responds.  But the
problem is that the 9P connection the outer agent uses is the
SAME pipe that serves the agent's own conversation.  The
`srvrelease`/`srvacquire` mechanism in lib9p allows concurrent
requests on different fids, but when the agent's tool execution
is itself a 9P client of its own server, the server may not be
able to process the new request until the current one completes
-- classic self-deadlock over a synchronous channel.

A second process on a separate pipe eliminates this entirely.

## Why namespace order matters (details)

On Plan 9, each process has its own namespace -- a private
mapping of paths to file trees.  Namespaces are inherited
from parent to child at fork/rfork time.  They are NOT
shared retroactively.

When you type `claude9fs -s claudesub -m /mnt/claudesub` in
your shell, only your shell's namespace (and future children)
gets the mount.  An already-running claude9fs process is
unaffected -- its namespace was fixed when it started.

claudetalk (the rc script) runs in your shell's namespace,
so it can see everything.  But the tool calls that the agent
makes (read_file, create_file, etc.) are executed by the
outer claude9fs process, not by claudetalk.  So it is the
outer claude9fs's namespace that matters, not the shell's.

This is the same Plan 9 namespace isolation that makes
`mount -b '#|/data' /dev` work -- and the same reason that
`ns` output from one process doesn't describe what another
process sees.

## Testing log

- **2025-07-11:** First successful sub-agent test using a
  single process.  Likely worked because the clone was
  reached through a separate 9P connection (e.g. via /srv).

- **2025-07-14 (session gloomy-hammer):** Reading
  `/mnt/claude/clone` from the agent's tool loop produced
  "i/o on hungup channel", confirming the self-deadlock.
  Started second claude9fs with `-s claudesub -m /mnt/claudesub`
  but the outer claude9fs was already running -- it could
  not see the mount.  `list_directory /mnt/claudesub`
  returned empty.

- **2025-07-14 (session jazzy-weasel):** Same result --
  `/mnt/claudesub` visible as a mount point (inherited from
  shell) but file tree empty because the outer claude9fs
  started before the sub-agent server.  Root cause confirmed:
  namespace inheritance requires correct startup order.

## Notes

- **Two servers, many sub-agents:** The sub-agent server at
  `/mnt/claudesub` can host many sessions via `clone`.  You
  only need one extra process, not one per sub-agent.

- **Model per session:** Each session has its own `model` file.
  The outer agent can run on Opus while sub-agents run on Haiku
  to save costs.

- **UTF-8 artifacts:** Text round-tripping through the API can
  mangle multi-byte characters (em-dashes, curly quotes).
  Clean up or use ASCII when depositing into acme.

- **Streaming:** For long sub-agent outputs, you could read
  `stream` instead of `prompt` for incremental deltas, but
  `prompt` is simpler for batch use.

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
