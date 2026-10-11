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

Resource limits are local byte/count limits, not provider token budgets:

| Resource | Limit |
| --- | --- |
| Retained conversation | 7 MiB of compact serialized JSON |
| Outbound request | 8 MiB of JSON, including tools and settings |
| Inbound response | 8 MiB of total SSE body, including framing/comments |
| Interactive input line / session file read | 8 MiB |
| File read or edit input | 256 KiB of text, no NUL bytes |
| Shell output / directory listing | 64 KiB before UTF-8 repair and truncation notices |
| Tool calls per response | 16 |
| API calls per task | 24 by default; `--max-turns` accepts 1–3600 |
| Shell deadline | 120 seconds by default; `--command-timeout` accepts 1–3600 |
| API connection / total deadline | 30 / 300 seconds |
| Requested model output | 8192 tokens per response |
| Root `AGENTS.md` read | 32 KiB |

The conversation budget leaves room for the request envelope. A full conversation
stops the task with an error; earlier messages are not silently deleted. Use
`/clear` or start a new session. Assistant calls and reserved tool replies must fit
before any calls execute. If a tool's result would exceed the budget after it runs,
the saved reply retains `ok` and sets `output_omitted`; remaining calls are marked
unexecuted, and the task stops. File changes already made are not rolled back.
Sessions are written as compact JSON, within the loader's limit. Older sessions
above the conversation budget are rejected without rewriting them. Oversized input
lines are discarded; oversized responses abort without executing their calls.

These are not an 8 MiB process-memory ceiling: JSON nodes, strings, serialization
buffers, temporary results, libc, and curl add overhead. Request serialization
borrows the conversation tree instead of duplicating it. Buffers and the JSON
printer have bounded growth; out-of-memory allocation still exits. The provider's
context limit may be reached before the local byte limit.

There is no automatic context compaction,
background task manager, subagents, multimodal input, MCP, OAuth/subscription login,
or full-screen editor in this version. Use an API key and start a new conversation
or `/clear` when needed. Sessions are opt-in JSON files with mode 0600.

An agent's assertions do not prove correctness. Acceptance tests independently
compile/run the changed program and inspect the outputs, in addition to reviewing
the actual tool calls.
