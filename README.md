# gucode

A small C coding agent for macOS and gucOS. It streams DeepSeek responses, reads
and edits files, runs shell commands, and resumes saved conversations. The same
application sources use native libcurl on macOS and the HTTP-only gucOS libcurl
package in the browser. The application has no JavaScript runtime dependency.

## macOS

```sh
make
export DEEPSEEK_API_KEY='your-key'
./build/gucode -C /path/to/project
./build/gucode -C /path/to/project -p 'Find and fix the failing test'
```

Use `--key-file /path/to/private-key` instead of an environment variable if desired.
`--model` defaults to `deepseek-flash`; `--endpoint` defaults to DeepSeek's Chat
Completions endpoint. Only DeepSeek has been targeted for acceptance; other
providers are not claimed compatible.

File edits and shell commands ask for permission. `--yes` explicitly permits them
without prompts, including for noninteractive tasks. **Shell commands are not
sandboxed:** they execute with your account's permissions. File tools resolve paths
inside the selected working directory. API credentials loaded from the environment
are removed before launching commands, but this is not a security sandbox.

```sh
./build/gucode --session conversation.json
./build/gucode --yes --max-turns 16 -p 'Implement the requested change and test it'
```

Sessions are opt-in, stored with mode 0600, and may contain source code and command
output. The session path is relative to the working directory after `-C`. The key
file path is resolved before `-C`. Keep sessions out of Git. Interactive commands:
`/help`, `/clear`, `/exit`. Ctrl-C cancels a request or command and exits. ANSI color
is used only on a terminal and can be disabled with `--no-color` or `NO_COLOR`.

## gucOS

The public gucOS package catalog provides the same C application and its libcurl
subset dependency:

```sh
gucman install groundupcoder:gucode
gucode --key-file /home/user/deepseek.key
```

Create/upload your private API key file first. The package supports platform
1.0.1 or compatible later 1.x versions. For the library alone, install
`groundupcoder:libcurl`; its README defines the supported curl API.

See [architecture and limits](docs/design.md), [transport subset](docs/transport.md)
and [testing](docs/testing.md). MIT licensed; vendored cJSON retains its own license.
