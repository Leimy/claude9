# File Actions for claude9

## Status: Implemented

The action system allows Claude to create, delete, and modify files
through structured action blocks embedded in responses.

## Action Block Format

Three action types are supported:

### Create
```
<<<ACTION file:create path:foo/bar.c
full file contents here
>>>ACTION
```
Creates the file with the given contents. Parent directories are
created automatically. Always provide complete file contents.

### Delete
```
<<<ACTION file:delete path:foo/bar.c
>>>ACTION
```
Removes the specified file.

### Patch
```
<<<ACTION file:patch path:foo/bar.c
--- a/foo/bar.c
+++ b/foo/bar.c
@@ -10,3 +10,4 @@
 context line
-old line
+new line
+added line
>>>ACTION
```
Applies a unified diff to an existing file. Supports multiple hunks
and fuzzy context matching (±3 lines) for robustness against minor
line number drift. Preferred over file:create for small changes.

## Commands

- `/apply` or `/apply all` — apply all pending actions from the last reply
- `/apply N` — apply only action number N

Actions are displayed with numbers after each response:
```
actions:
  [1] create foo/bar.c (15 lines)
  [2] patch baz.c (8 lines diff)
  [3] delete old.c
use /apply to apply, /apply N for selective
```

## Safety

- Absolute paths (starting with `/`) are rejected
- Paths containing `..` are rejected
- Actions are never applied automatically; the user must explicitly `/apply`

## System Prompt

The default system prompt instructs Claude on the action block format.
A custom system prompt can be provided with `-s`, but it will replace
the action format instructions — users providing custom prompts should
include the action format if they want file operations.

## Implementation

- `action.c` — parsing, display, and application of actions (including patch)
- `claude.c` — default system prompt with action format instructions
- `chat.c` — `/apply` command handler, action detection in replies
