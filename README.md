# claude9

An interactive Claude AI client for Plan 9 from Bell Labs.

claude9 consists of three components:

- **claude9** - a terminal-based chat interface with file actions,
  conversation save/load, and bead-based context chaining.
- **claude9fs** - a 9P filesystem server that exposes Claude sessions
  as files, suitable for scripting and integration with other tools.
- **claudetalk** - an rc shell script that provides an interactive
  chat interface via claude9fs.

All components use the Anthropic Claude API over HTTP.

## Building

### Plan 9 / 9front

Build with mk:

	mk
	mk install

This builds both claude9 and claude9fs and installs them to
$home/bin/$objtype.

### plan9port

Build with mk using the plan9port makefile:

	mk -f mkfile.plan9port

This builds claude9 (the interactive chat client) and installs
it to $HOME/bin. claude9fs requires the Plan 9 thread and 9p
libraries and is not built under plan9port.

## Setup

Set your Anthropic API key in the environment:

	ANTHROPIC_API_KEY=sk-ant-...

HTTP requests are made via webfs (/mnt/web). Make sure webfs is mounted.
On systems without webfs (e.g. plan9port), curl is used as a fallback.

## claude9 - Interactive Chat

	claude9 [-m model] [-s sysprompt] [-t maxtokens]

Flags:

	-m model       model name or alias (default: claude-opus-4-6)
	-s sysprompt   custom system prompt
	-t maxtokens   maximum output tokens (default: 65536)

Messages are entered at the prompt and terminated by a line containing
only a period (.). Single-line commands starting with / are executed
immediately.

### Commands

	/help                                   show available commands
	/models                                 list available models from the API
	/model                                  show current model
	/model <name>                           switch model (alias or full ID)
	/read <file> [file ...] [-- comment]    read files into context and send
	/save <file>                            save conversation to file
	/load <file>                            load conversation from file
	/beadsave <file>                        save condensed bead (summary + file refs)
	/beadload <file>                        load bead, re-reading files from disk
	/clear                                  clear conversation history
	/tokens                                 show current max tokens
	/tokens <n>                             set max tokens
	/apply [all|N]                          apply file actions from last response

### File Actions

Claude can propose file changes using structured action blocks in its
responses. These are parsed automatically and presented for review.
Use "/apply all" to apply all proposed changes, or "/apply N" to apply
a specific one.

Supported action types:

	file:create    create a new file with the given contents
	file:delete    remove a file
	file:patch     apply a unified diff to an existing file

Patches are applied with fuzzy context matching (up to 3 lines of fuzz)
and whitespace-tolerant line comparison.

### Beads

The /beadsave and /beadload commands implement a lightweight context
chaining mechanism. A bead captures a Claude-generated summary of the
conversation along with references to the files discussed. When loaded
with /beadload, the files are re-read from disk, giving Claude fresh
context without replaying the entire conversation history.

## claude9fs - 9P Filesystem

	claude9fs [-s srvname] [-m mtpt] [-M model] [-t maxtokens]

Flags:

	-s srvname     post to /srv with this name
	-m mtpt        mount point (default: /mnt/claude)
	-M model       default model (default: claude-sonnet-4-20250514)
	-t maxtokens   default max tokens (default: 16384)

claude9fs serves a 9P filesystem where each Claude conversation is
a numbered directory. Reading clone allocates a new session and returns
its number.

### Filesystem Layout

	/mnt/claude/
		clone         read to create a new session (returns session ID)
		models        read to list available models from the API
		<n>/          session directory
			ctl       read for session info; write commands
			prompt    write a message, read the reply
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

When accessed through claude9fs, Claude has access to file tools
(create_file, patch_file, read_file, list_directory, delete_file)
which are executed automatically during conversation turns. The full
tool loop runs when writing to prompt: Claude's tool calls are
executed and results sent back until Claude produces a final response.

## claudetalk - RC Shell Client

	claudetalk [-m model] [-t tokens]

claudetalk is an rc script that provides an interactive chat interface
using claude9fs. It requires claude9fs to be mounted on /mnt/claude.

### Commands

	/models        list available models
	/model         show current model
	/model <name>  switch model
	/clear         clear conversation
	/status        show session info
	/usage         show token usage
	/save <path>   save conversation
	/load <path>   load conversation
	/quit          exit

Messages are entered and sent with ^D (end of file) on an empty line.

## Example Session (claude9)

	% claude9
	claude9 - opus4.6 (claude-opus-4-6)
	type /help for commands, end messages with '.' on its own line

	opus4.6/65536> /read main.c util.c -- explain this code
	  added main.c
	  added util.c
	sending 2 messages (4832 bytes)... 8s
	[tokens: 1200 in, 350 out, 1550 total, stop: end_turn]
	...

	opus4.6/65536> refactor the error handling
	.

	...
	actions:
	  [1] patch main.c (12 lines diff)
	  [2] patch util.c (8 lines diff)
	use /apply to apply, /apply N for selective

	opus4.6/65536> /apply all
	  patched main.c (1 hunks)
	  patched util.c (1 hunks)

## Example Session (claudetalk)

	% claude9fs -s claude
	% claudetalk
	claude9 session 0 - claude-sonnet-4-20250514
	commands: /models /model [name] /clear /status /usage /help /quit
	type message, end with ^D on empty line, ^C to quit

	claude-sonnet-4-20250514/16384> hello
	^D
	...
	Hello! How can I help you today?

	claude-sonnet-4-20250514/16384> /usage
	input_tokens 12
	output_tokens 10
	total_tokens 22
	stop_reason end_turn

## File Format

Saved conversations use a simple line-oriented text format with headers
for metadata (model, maxtokens, sysprompt) and message separators.
Lines in message bodies that start with "---" are escaped with a
leading space.

## Source Files

	claude.c       API client: conversation management, HTTP, tool execution
	claude.h       shared data structures and function declarations
	json.c         JSON parser and serializer
	json.h         JSON type definitions
	chat.c         interactive terminal client (claude9)
	action.c       action block parser and patch application for claude9
	claude9fs.c    9P filesystem server (claude9fs)
	claudetalk     rc script client for claude9fs

## Dependencies

	Plan 9 C compiler and libraries (libc, bio)
	thread and 9p libraries (for claude9fs, Plan 9 only)
	webfs for HTTP (or curl as fallback on plan9port)
	Anthropic API key

## License

MIT License. See the LICENSE file for details.
