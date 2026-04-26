# claude9

A Claude AI client for Plan 9 from Bell Labs, exposed as a 9P
filesystem.

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

There is no sandbox.  Tool calls run with the full authority of
the user that started claude9fs.  On Plan 9 that typically means
your whole home tree, anything mounted in your namespace
(including `/mnt/term/...` if you are a drawterm/cpu client into
your own files), and on plan9port it means your real Unix user
account.

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

- Run claude9fs as an unprivileged user, ideally in a
  namespace where only the trees you actually want it to touch
  are visible (`rfork n`, bind only what is needed, then
  `mk install`).
- Keep the source trees you let it edit under version control
  and commit before each session.
- Do not run it as a user that has your long-term secrets in
  readable files.
- Treat anything the model is asked to read - web pages,
  issue trackers, third-party source - as potentially
  hostile input that may try to redirect its tool use.
- Read `claude.c` if you want to see exactly what each tool
  does; it is short.

## Building

### Plan 9 / 9front

	mk
	mk install

This builds claude9fs and installs it to `$home/bin/$objtype`.
The `claudetalk` rc script is installed to `/rc/bin`, which is
the canonical location for system-wide rc scripts on Plan 9 and
9front; `mk install` therefore needs write permission there
(run it as the appropriate user, e.g. `glenda` on a default
9front install, or adjust `RCBIN` in the mkfile).

### plan9port (Mac / Linux)

	mk -f mkfile.plan9port
	mk -f mkfile.plan9port install

This builds claude9fs against plan9port's libthread and lib9p.
On systems without webfs, claude9fs falls back to invoking
`/usr/bin/curl` for HTTP, so streaming is delivered in one burst
at end of round rather than incrementally.  Everything else
works the same.

## Setup

Set your Anthropic API key in the environment:

	ANTHROPIC_API_KEY=sk-ant-...

HTTP requests prefer webfs (`/mnt/web`) when available; on
systems without webfs, curl is used.

## claude9fs - 9P Filesystem

	claude9fs [-s srvname] [-m mtpt] [-M model] [-t maxtokens]

Flags:

	-s srvname     post to /srv with this name
	-m mtpt        mount point (default: /mnt/claude)
	-M model       default model (default: claude-sonnet-4-20250514)
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

	clear          clear conversation history
	hangup         destroy the session
	save <path>    save conversation to a file
	load <path>    load conversation from a file

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
hits EOF.  See `STREAMING.md` for details.

To see the last round's text after the fact, read `prompt`
instead.

## claudetalk - rc Shell Client

	claudetalk [-m model] [-t tokens]

claudetalk is an rc script that talks to a running claude9fs
mounted on `/mnt/claude`.  It prints incremental text by reading
the session's `stream` file in the background while writing to
`prompt`.

### Commands

	/models        list available models
	/model         show current model
	/model <name>  switch model
	/tokens        show current max tokens
	/tokens <n>    set max tokens
	/clear         clear conversation
	/status        show session info
	/usage         show token usage
	/save <path>   save conversation
	/load <path>   load conversation
	/quit          exit

Messages are entered and sent with `^D` on an empty line.

## Example Session

	% claude9fs -s claude
	% claudetalk
	claude9 session 0 - claude-sonnet-4-20250514
	commands: /models /model [name] /clear /status /usage /help /quit
	type message, end with ^D on empty line, ^C to quit

	claude-sonnet-4-20250514/16384> hello
	^D
	Hello! How can I help you today?

	claude-sonnet-4-20250514/16384> /usage
	input_tokens 12
	output_tokens 10
	total_tokens 22
	stop_reason end_turn

## File Format

Saved conversations use a simple line-oriented text format with
headers for metadata (model, maxtokens, sysprompt) and message
separators.  Lines in message bodies that start with `---` are
escaped with a leading space.

## Source Files

	claude.c       API client: conversation, HTTP (webfs/curl), tool execution
	claude.h       shared data structures and function declarations
	patch.c        in-tree fuzzy unified-diff applier (patch_file tool)
	json.c         JSON parser and serializer
	json.h         JSON type definitions
	claude9fs.c    9P filesystem server (claude9fs)
	claudetalk     rc script client for claude9fs

## Dependencies

	C compiler and Plan 9 / plan9port libraries (libc, bio, thread, 9p)
	webfs for streaming HTTP (or curl as fallback)
	Anthropic API key

## License

MIT License.  See the LICENSE file for details.
