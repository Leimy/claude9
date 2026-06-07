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

	claude9fs [-K skillsdir] [-s srvname] [-m mtpt] [-M model] [-t maxtokens]

Flags:

	-K skillsdir   load skill files from this directory into the system prompt
	-s srvname     post to /srv with this name
	-m mtpt        mount point (default: /mnt/claude)
	-M model       default model (default: claude-opus-4-6)
	-t maxtokens   default max tokens (default: 16384)

claude9fs serves a 9P filesystem where each Claude conversation
is a numbered directory.  Reading `clone` allocates a new session
and returns its number.

### Filesystem Layout

	/mnt/claude/
		clone         read to create a new session (returns session number)
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

	clear              clear conversation history and usage counters
	hangup             destroy the session
	autocontinue [n]   enable auto-continue (default n=3)
	noautocontinue     disable auto-continue

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
much safer than line-number-based editing, which silently
corrupts files when line numbers are stale or wrong.

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

### Skills

The `-K` flag points claude9fs at a directory of skill files.
At startup, every regular file in that directory is read and
its contents are appended to the system prompt under a
`Skills` heading.  Each file becomes a subsection named after
the file.  This lets you configure persistent instructions --
coding conventions, project context, persona tweaks -- without
editing C or passing a giant `-sysprompt` string.

Skills are baked into the system prompt at startup, so they
benefit from Anthropic's prompt caching: the entire skills
block is a cache hit on every request after the first, adding
no extra latency or cost.

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
prompt in every session.  To change skills, edit the files and
restart claude9fs.

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

Skills are appended after the default system prompt (or after
a custom one if you write to the session's `system` file
before loading skills).  Subdirectories are ignored; only
regular files are read.

## claudetalk - rc Shell Client

	claudetalk [-d] [-a session] [-M model] [-t maxtokens] [-K skillsdir]

Flags:

	-a n      attach to existing session n (instead of cloning a new one)
	-d        detach on exit: leave session alive for later reattachment
	-M model  default model (passed through to claude9fs)
	-t n      default max tokens (passed through to claude9fs)
	-K dir    skills directory (passed through to claude9fs)

claudetalk is an rc script that bootstraps the claude9fs
environment (including a sub-agent server for sub-agent
support) and provides an interactive chat interface.  It
prints incremental text by reading the session's `stream`
file in the background while writing to `prompt`.

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
	/autocontinue [n]  enable auto-continue on max_tokens (default 3)
	/noautocontinue    disable auto-continue
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

## Source Files

	claude.c       API client: conversation, HTTP via webfs, tool execution
	claude.h       shared data structures and function declarations
	json.c         JSON parser and serializer
	json.h         JSON type definitions
	claude9fs.c    9P filesystem server
	claudetalk     rc script client

## Dependencies

	9front
	webfs (for HTTP to the Anthropic API)
	Anthropic API key

## License

MIT License.  See the LICENSE file for details.
