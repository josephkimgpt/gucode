# Testing

Run builds and test suites under the shared machine-job scheduler when installed:

```sh
machine-job run --kind build --timeout 300 -- make
machine-job run --kind build --timeout 300 -- python3 tests/regression.py
machine-job run --kind build --timeout 300 -- make check sanitize
machine-job run --kind build --timeout 300 -- make check-leaks  # macOS
machine-job run --kind build --timeout 600 -- python3 tests/live.py
```

Without that machine-wide scheduler (ordinary installations), `make` and the
Python test commands run directly. The application itself requires only C and
libcurl; Python is a host test driver, not an application dependency.

The live test is opt-in and makes paid DeepSeek requests. Set DEEPSEEK_KEY_FILE to
a private credential file. Tests use disposable working directories; durable
reports contain task outcomes and tool names, never keys or conversation bodies.
The deterministic suite covers malformed/truncated responses, split SSE/UTF-8,
permission denial, workspace confinement, edit match cardinality, command timeout,
output bounds, and removal of the API-key environment variable from child tools.

`make check` also runs `tests/resources.py`: repeated tool requests and `/clear`,
aggregate response limits, oversized history/input and UTF-8 expansion, bounded
tool replies, session resume, and persistence errors. All use a loopback mock
server, a disposable HOME, and synthetic credentials; they incur no API charges.
`make sanitize` runs both suites under AddressSanitizer and UndefinedBehaviorSanitizer.
`make check-leaks` additionally runs macOS `leaks --atExit` for a 40-task tool
conversation and session error paths, requiring explicit zero-leak reports.
The leak checker reports its own exit status, so target error exit codes are also
checked without the wrapper. This is sampled leak evidence, not proof that every
possible execution is leak-free; retained history and allocator high-water marks
must also be distinguished from unreachable allocations.

The gucOS package repository owns browser build, source install, retained archive
relinking, binary install, reload and live API acceptance. It takes an explicit
core distribution and runtime identity; no core build reads the packages checkout.

## 0.1.0 release evidence

Native reports are under `docs/research/deepseek/`. The final public gucOS
[acceptance reports](https://github.com/GroundUpCoder/gucos-packages/tree/a690713/docs/research/gucode)
include the successful interactive DeepSeek write and exit, exact payload
verification, prior failures and scheduler exit status. The public terminal run
did not emit ANSI color. Only DeepSeek was used for live provider acceptance.

## Interactive input (0.1.2)

`tests/input.py` drives an actual native PTY against an offline DeepSeek-shaped
fixture. It checks exact submitted text for cursor movement, Home/End, forward
and backward deletion, kill/yank, word erase, history draft preservation, Unicode,
horizontal scrolling and resize. It covers bracketed paste, malformed UTF-8 before
a paste terminator, and cancellation of an unfinished paste. Exit, EOF, Ctrl-C
and SIGTERM restore terminal attributes; Darwin's kernel-managed PENDIN bit is
excluded from comparison. The PTY is drained while the process runs so terminal
output backpressure is not mistaken for an editor failure.

The editor supports Ctrl-A/E/B/F, Ctrl-P/N or Up/Down, Ctrl-U/K/W/Y, Ctrl-D,
Ctrl-L, Home/End/Delete/Backspace and Alt-B/F. History stays in process memory,
with at most 32 entries of up to 8 KiB each. Prompt input retains the 8 MiB bound;
an overflow rejects that submission. Bracketed pasted newlines become spaces;
Ctrl-C can cancel incomplete paste. Input is edited on one horizontally scrolling
line. UTF-8 codepoint editing is supported; combining grapheme clusters are not
claimed as indivisible editing units. Ctrl-C exits the application as before.
Terminal settings are restored before any tool/HTTP execution or confirmation
transition. Pipe input retains its plain line-reading behavior.
