# Design and limitations

The executable consists of C code plus vendored cJSON 1.7.19. `http.c` sends
DeepSeek Chat Completions requests using the documented curl subset. An SSE parser
accumulates arbitrary byte fragments and assembles complete tool calls before any
execution. Non-thinking mode avoids provider-specific reasoning-state handling.
A successful turn requires a valid finish reason and `[DONE]`; truncated responses
are errors. API errors are not automatically retried, so paid requests and tool
side effects cannot be replayed silently.

`tools.c` implements read_file, list_files, write_file, edit_file and shell. Edits
require exactly one matching old string. Writes stage a sibling temporary file
and rename it atomically, preserving existing permissions. File tools resolve
paths under the working directory; symlink escapes and the selected key/session
files (including inode aliases) are refused. This is a convenience boundary, not
a defense against a concurrently hostile local process. Approved shell commands
have the user's full permissions and can access other files.

Commands run through posix_spawn with a separate process group, no stdin, and
combined captured stdout/stderr. Output is capped at 64 KiB; commands default to a
120-second deadline. Ctrl-C cancels and exits. Remaining descendants in that group
are killed on completion. Processes that deliberately escape their group are not
contained; this program is not a sandbox. Output draining is bounded so constant
output cannot starve timeout checks. Tool results are displayed with control
characters escaped to prevent ANSI command injection. Application color is emitted
separately. Terminal wrapping and basic input editing are supplied by the terminal.

Requests have a five-minute total deadline. Input/session buffers are capped at
8 MiB; JSON allocation failure exits before executing a partially built tool batch.
Text file reads are limited to 256 KiB. There is no automatic context compaction,
background task manager, subagents, multimodal input, MCP, OAuth/subscription login,
or full-screen editor in this version. Use an API key and start a new conversation
or `/clear` when needed. Sessions are opt-in JSON files with mode 0600.

An agent's assertions do not prove correctness. Acceptance tests independently
compile/run the changed program and inspect the outputs, in addition to reviewing
the actual tool calls.
