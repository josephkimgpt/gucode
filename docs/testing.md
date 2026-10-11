# Testing

Run builds and test suites under the shared machine-job scheduler when installed:

```sh
machine-job run --kind build --timeout 300 -- make
machine-job run --kind build --timeout 300 -- python3 tests/regression.py
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

The gucOS package repository owns browser build, source install, retained archive
relinking, binary install, reload and live API acceptance. It takes an explicit
core distribution and runtime identity; no core build reads the packages checkout.

## 0.1.0 release evidence

Native reports are under `docs/research/deepseek/`. The final public gucOS
[acceptance reports](https://github.com/GroundUpCoder/gucos-packages/tree/a690713/docs/research/gucode)
include the successful interactive DeepSeek write and exit, exact payload
verification, prior failures and scheduler exit status. The public terminal run
did not emit ANSI color. Only DeepSeek was used for live provider acceptance.
