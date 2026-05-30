# Skill: Coordinating a Sub-Agent via claude9fs and Acme

## What this does

Spawn a sub-agent as a second session on the same claude9fs,
send it a task, and deposit the result in an acme window --
all using ordinary file reads and writes from the outer agent's
tool loop.

No second claude9fs process is needed.  claude9fs supports
multiple concurrent sessions; each is an independent directory
under the mount point.  The outer agent (your current session)
and the sub-agent are just two sessions on the same server.

## Prerequisites

Start acme *before* claude9fs so that `/mnt/acme` is in the
namespace when claude9fs inherits it:

    % acme
    % claude9fs          # inherits /mnt/acme from acme's namespace

The agent now has both `/mnt/claude` (its own claude9fs) and
`/mnt/acme` available as files it can read and write.

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

Read `clone` on the *same* claude9fs mount to allocate a new
session.  This returns a session name:

    read_file /mnt/claude/clone   -->  "dizzy-monkey"

This session is independent of the outer agent's session.
They share the server process and API key but have separate
conversation histories, models, and system prompts.

### 3. Configure the session

Set the model to something cheap for simple tasks:

    create_file /mnt/claude/dizzy-monkey/model "claude-haiku-4-5-20251001"

Optionally set a system prompt to keep the sub-agent focused:

    create_file /mnt/claude/dizzy-monkey/system "You are a poet. Respond only with the poem."

### 4. Send a prompt and read the reply

Write the user message to `prompt`.  The write blocks until
the model finishes (including any tool-use loops):

    create_file /mnt/claude/dizzy-monkey/prompt "Write a poem about X."

Read `prompt` back to get the last assistant reply:

    read_file /mnt/claude/dizzy-monkey/prompt   -->  (the poem)

### 5. Deposit the result in the acme window

Write the text to `/mnt/acme/<id>/body`.  Writes to `body`
are always appended (file offset is ignored):

    create_file /mnt/acme/14/body "<the poem text>"

The text appears immediately in the acme window.

### 6. Clean up

Hang up the sub-agent session:

    create_file /mnt/claude/dizzy-monkey/ctl "hangup"

## Verified workflow (2025-07-11)

This exact sequence was tested successfully:

1. acme was started first
2. claude9fs was started (inheriting acme's namespace)
3. The outer agent created an acme window named `/tmp/llm-poem`
4. The outer agent read `/mnt/claude/clone` to get session `dizzy-monkey`
5. Set the session model to `claude-haiku-4-5-20251001`
6. Set a system prompt ("You are a poet...")
7. Wrote a prompt asking for a poem about writing code with LLMs
8. Read back the poem from `prompt`
9. Wrote the poem text to `/mnt/acme/14/body`
10. Hung up the sub-agent session

The whole thing used one claude9fs process with two sessions.

## Notes

- **One server, many agents:** You do not need a second
  claude9fs.  Each `clone` creates an independent session.
  You could spawn several sub-agents in parallel (e.g. one
  for research, one for code review) on the same mount.

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
