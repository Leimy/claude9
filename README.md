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

## Building

### Plan 9 / 9front

	mk
	mk install

This builds claude9fs and installs it (along with the claudetalk
script) to `$home/bin/$objtype`.

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
