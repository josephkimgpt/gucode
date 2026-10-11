# gucode 0.1.1

Added `$HOME/.guc/creds/deepseek-api-key` as the first credential source,
followed by the existing explicit file and environment fallbacks. Only missing
files fall back; unreadable/invalid files fail without exposing their contents.
The selected default file receives the existing file-tool alias protections.
`--yolo` aliases `--yes` for both file edits and shell commands.

Validation: `machine-job run --kind build --timeout 300 -- make check sanitize`
passed all 24 regression groups for release and ASan/UBSan binaries. Six existing
vendored cJSON deprecation warnings; no sanitizer diagnostics. Tests use synthetic
keys and a loopback SSE fixture, not live DeepSeek. Independent read-only agent
review found no blockers. Guest package acceptance is recorded in gucos-packages.
