# gucode 0.1.1

Added `$HOME/.guc/creds/deepseek-api-key` as the first credential source,
followed by the existing explicit file and environment fallbacks. Only missing
files fall back; unreadable/invalid files fail without exposing their contents.
The selected default file receives the existing file-tool alias protections.
`--yolo` aliases `--yes` for both file edits and shell commands.

Validation: `machine-job run --kind build --timeout 300 -- make check sanitize`
passed all 26 regression groups and nine resource-test groups for release and ASan/UBSan binaries. Six existing
vendored cJSON deprecation warnings; no sanitizer diagnostics. Tests use synthetic
keys and a loopback SSE fixture, not live DeepSeek. Independent read-only agent
review found no blockers. Guest package acceptance is recorded in gucos-packages.


The release also fixes malformed UTF-8 from binary shell/file output entering
API JSON. Valid Unicode is preserved; invalid bytes become U+FFFD, including
malformed strings/member names in resumed conversations. The raw byte capture
limit remains 65536 bytes; replacement text may be longer. Terminal previews end
on a character boundary. Native tests include malformed sequences, raw binary,
Unicode across pipe and preview boundaries, output-limit truncation and recovery
of a previously damaged session. The guest acceptance harness includes `head -c 2000 /bin/cc`.


After the requested pull, this release includes upstream commit 02c4059's
conversation/request/response limits and preservation of resumable tool replies.
Independent review found a normalization collision between malformed tool IDs;
a final uniqueness check now rejects the entire response before tool execution.
The id_collision regression verifies that neither proposed write runs. The full
native and sanitizer suites passed again with these combined changes.
