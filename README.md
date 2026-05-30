# claude9

A Claude AI client for 9front, exposed as a 9P filesystem.

claude9 consists of two components:

- **claude9fs** - a 9P filesystem server that exposes Claude
  sessions as files, suitable for scripting and integration with
  any tool that can read and write files.
- **claudetalk** - an rc shell script that drives claude9fs to
  give you an interactive chat interface in the terminal.

Together they replace the older standalone `claude9` chat
client; everything that program did can be done through the
filesystem instead, and the filesystem composes with the rest
of the system in the usual Plan 9 way.

## Safety and risks - read this first

**claude9fs gives the model the ability to read, write, patch,
and delete files on your machine.**  When you write to `prompt`,
claude9fs runs a tool loop in which Claude can call any of the
following tools without asking you for confirmation each time:

- `create_file` - create or overwrite a file at any path the
  process can write to.
- `patch_file` - apply a fuzzy unified diff to an existing file.
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
  private mail) and sending their contents to Anthropic as
  part of a tool result;
- following an instruction smuggled into a document
  ("prompt injection") that turns a request like "summarise
  this file" into "delete the following files".

It cannot escape the permissions of the user running
claude9fs, and it does not get raw shell or network access -
the only side effects are the five file tools above plus the
HTTPS calls claude9fs itself makes to the Anthropic API.  But
within those limits it can do real damage.

### Recommendations

- **Sandbox the namespace.**  Run claudetalk (and its
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
- Read `claude.c` if you want to see exactly what each tool
  does; it is short.
- For a stronger boundary, run the whole thing as a dedicated
  user whose only file permissions are on the project tree.

## Building

	mk
	mk install

This builds claude9fs and installs it to `$home/bin/$objtype`.
The `claudetalk` rc script is installed to `/rc/bin`, which is
the canonical location for system-wide rc scripts on 9front;
`mk install` therefore needs write permission there (run it as
the appropriate user, e.g. `glenda` on a default 9front install,
or adjust `RCBIN` in the mkfile).

## Setup

Set your Anthropic API key in the environment:

	ANTHROPIC_API_KEY=sk-ant-...

HTTP is handled via webfs (`/mnt/web`), which must be mounted.

## claude9fs - 9P Filesystem

	claude9fs [-s srvname] [-m mtpt] [-M model] [-t maxtokens]

Flags:

	-s srvname     post to /srv with this name
	-m mtpt        mount point (default: /mnt/claude)
	-M model       default model (default: claude-opus-4-6)
	-t maxtokens   default max tokens (default: 16384)

claude9fs serves a 9P filesystem where each Claude conversation
is a numbered directory.  Reading clone allocates a new session
and returns its number.

### Filesystem Layout

	/mnt/claude/
		clone         read to create a new session (returns session ID)
		models        read to list available models from the API
		<n>/          session directory
			ctl       read for session info; write commands
			prompt    write a message; read the last final reply
			stream    read incremental text deltas of current round
			conv      read full conversation history
			model     read/write the model name
			tokens    read/write max output tokens
			system    read/write the system prompt
			usage     read token usage statistics
			error     read last error message

### Session Commands (write to ctl)

	clear          clear conversation history and usage counters
	hangup         destroy the session
	save <path>    save conversation to a file
	load <path>    load conversation from a file
	autocontinue [n]  enable auto-continue (default n=3)
	noautocontinue    disable auto-continue

### Tool Use

When you write to `prompt`, claude9fs runs the full tool loop:
Claude has access to file tools (`create_file`, `patch_file`,
`read_file`, `list_directory`, `delete_file`) which are executed
automatically as part of the round, with results sent back to
Claude until it produces a final response.

The `patch_file` tool uses the in-tree fuzzy unified-diff applier
(see `patch.c`), which tolerates off-by-one line numbers, missing
`---`/`+++` headers, and minor whitespace drift.

### Streaming

The `stream` file emits text deltas incrementally as the model
generates them.  A reader opened against an idle session blocks
until the next round starts; once the round finishes, the reader
hits EOF.

To see the last round's text after the fact, read `prompt`
instead.

### Auto-Continue

When Claude's response hits the `max_tokens` limit, the API
returns `stop_reason: max_tokens` and the output is truncated
mid-sentence.  Normally you would need to manually send a
follow-up message to get the rest.

The `autocontinue` ctl command automates this.  When enabled,
claude9fs checks the stop reason after each response; if it is
`max_tokens`, the fs automatically appends a `"Continue."` user
message and sends another request, keeping the stream open so
the reader sees seamless text.  This repeats up to *n* times or
until the model stops on its own (e.g. `end_turn`).

	echo autocontinue > /mnt/claude/$n/ctl     # enable, default 3 rounds
	echo autocontinue 5 > /mnt/claude/$n/ctl   # enable, up to 5 rounds
	echo noautocontinue > /mnt/claude/$n/ctl   # disable

The current setting is shown in `ctl` output as
`autocontinue N` (0 means disabled).

Because continuations fire within seconds of each truncation,
the Anthropic prompt cache stays warm -- the entire conversation
prefix is a cache hit on each follow-up, so you only pay for
new output tokens.

Auto-continue is off by default.  Each continuation round
counts against the same usage totals shown in the `usage` file.

## claudetalk - rc Shell Client

	claudetalk [-d] [-a session] [-l convfile]

Flags:

	-a N      attach to existing session N (instead of cloning a new one)
	-d        detach on exit: leave session alive for later reattachment
	-l file   load a saved conversation file on startup

claudetalk is an rc script that talks to a running claude9fs
mounted on `/mnt/claude`.  It prints incremental text by reading
the session's `stream` file in the background while writing to
`prompt`.

### Commands

	/sessions      list live sessions (current session marked with *)
	/models        list available models
	/model         show current model
	/model <name>  switch model
	/tokens        show current max tokens
	/tokens <n>    set max tokens
	/clear         clear conversation
	/status        show session info
	/usage         show token usage
	/save <path>   save conversation to file
	/load <path>   load conversation from file
	/autocontinue [n]  enable auto-continue on max_tokens (default 3)
	/noautocontinue    disable auto-continue
	/detach        keep session alive on exit (can reattach later)
	/help          show command list
	/quit          exit

Messages are entered as text and sent with `^D`.

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
	claude9 session 0 - claude-opus-4-6
	claude-opus-4-6/16384> What files are in /sys/src/cmd?
	^D
	...
	/detach
	session 0 will persist on exit (reattach with: claudetalk -a 0)
	/quit

	# Window 2: pick up where we left off
	% claudetalk -a 0
	claude9 session 0 - claude-opus-4-6
	claude-opus-4-6/16384> Now look at /sys/src/cmd/ls.c
	^D
	...

#### Example: save and reload across restarts

	% claudetalk
	...
	/save /usr/dave/convos/refactor-session
	saved to /usr/dave/convos/refactor-session
	/quit

	# Later, or after restarting claude9fs:
	% claudetalk -l /usr/dave/convos/refactor-session
	loaded /usr/dave/convos/refactor-session
	...

#### Example: list and manage sessions

	% claudetalk -d
	claude9 session 1 - claude-opus-4-6
	/sessions
	sessions:
	  0 claude-opus-4-6 (14 msgs)
	  1 claude-opus-4-6 (0 msgs) *

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

## File Format

Saved conversations use a simple line-oriented text format with
headers for metadata (model, maxtokens, sysprompt) and message
separators.  Lines in message bodies that start with `---` are
escaped with a leading space.

## Source Files

	claude.c       API client: conversation, HTTP via webfs, tool execution
	claude.h       shared data structures and function declarations
	patch.c        in-tree fuzzy unified-diff applier (patch_file tool)
	json.c         JSON parser and serializer
	json.h         JSON type definitions
	claude9fs.c    9P filesystem server (claude9fs)
	claudetalk     rc script client for claude9fs

## Dependencies

	9front
	webfs (for HTTP to the Anthropic API)
	Anthropic API key

## License

MIT License.  See the LICENSE file for details.
