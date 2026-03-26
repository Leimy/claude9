# claude9

An interactive Claude AI client for Plan 9 from Bell Labs.

claude9 is a terminal-based chat interface to the Anthropic Claude API,
written in C for Plan 9. It supports multi-turn conversations, file
reading, conversation save/load, and automatic file creation and patching
via structured action blocks in Claude's responses.

## Building

Requires a Plan 9 system (or 9front). Build with mk:

	mk
	mk install

This installs claude9 to /$objtype/bin/.

## Setup

Set your Anthropic API key in the environment:

	ANTHROPIC_API_KEY=sk-ant-...

HTTP requests are made via webfs (/mnt/web). Make sure webfs is mounted.
On systems without webfs (e.g. plan9port), curl is used as a fallback.

## Usage

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

### Example Session

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

## File Format

Saved conversations use a simple line-oriented text format with headers
for metadata (model, maxtokens, sysprompt) and message separators.
Lines in message bodies that start with "---" are escaped with a
leading space.

## Dependencies

	Plan 9 C compiler and libraries (libc, bio)
	webfs for HTTP (or curl as fallback on plan9port)
	Anthropic API key

## License

MIT License. See the LICENSE file for details.
