#!/usr/bin/env python3
"""Offline resource limits, recovery, and optional macOS exit leak checks."""
import http.server
import json
import os
import pathlib
import re
import subprocess
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = os.environ.get("GUCODE_BINARY", str(ROOT / "build/gucode"))
MIB = 1024 * 1024
LEAKS = os.environ.get("GUCODE_LEAK_CHECK") == "1"


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, address):
        # Oversized streams are deliberately disconnected by the client.
        import sys
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, address)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        size = int(self.headers["Content-Length"])
        assert size <= 8 * MIB
        body = json.loads(self.rfile.read(size))
        self.server.requests += 1
        self.send_response(200)
        self.end_headers()

        def event(delta, finish=None):
            data = {"choices": [{"delta": delta, "finish_reason": finish}]}
            self.wfile.write(("data: " + json.dumps(data) + "\n\n").encode())

        def call(index, name, arguments, ident=None):
            return {"index": index, "id": ident or f"call_{index}",
                    "type": "function", "function": {"name": name,
                    "arguments": json.dumps(arguments)}}

        last = body["messages"][-1]
        prompt = last["content"]
        if last["role"] == "tool" or prompt == "short":
            event({"content": "done"}, "stop")
        elif prompt == "flood":
            event({"tool_calls": [call(0, "write_file", {
                "path": "never", "content": "must not execute"})]})
            for _ in range(2050):
                self.wfile.write(b":" + b"x" * 4094 + b"\n")
            event({}, "tool_calls")
        elif prompt == "large":
            event({"content": "x" * (7 * MIB)}, "stop")
        elif prompt == "large_id":
            event({"tool_calls": [call(0, "write_file", {
                "path": "never", "content": "must not execute"}, "x" * (4 * MIB))]},
                "tool_calls")
        elif prompt == "invalid_id":
            data = {"choices": [{"delta": {"tool_calls": [call(0, "write_file", {
                "path": "never", "content": "must not execute"}, "BAD_ID")]},
                "finish_reason": "tool_calls"}]}
            # Repair triples these bytes. Budget the repaired ID and its reply
            # before allowing the write, not when preparing the next request.
            wire = json.dumps(data).encode().replace(b"BAD_ID", b"\xff" * (1536 * 1024))
            self.wfile.write(b"data: " + wire + b"\n\n")
        elif prompt == "batch":
            event({"tool_calls": [
                call(0, "read_file", {"path": "large.txt"}),
                call(1, "write_file", {"path": "never", "content": "skipped"})]},
                "tool_calls")
        else:
            event({"tool_calls": [call(0, "read_file", {"path": "small.txt"})]},
                  "tool_calls")
        self.wfile.write(b"data: [DONE]\n\n")


server = Server(("127.0.0.1", 0), Handler)
server.requests = 0
threading.Thread(target=server.serve_forever, daemon=True).start()
passed = []
try:
    with tempfile.TemporaryDirectory(prefix="gucode-resources-") as td:
        work = pathlib.Path(td)
        (work / "small.txt").write_text("small result")
        (work / "large.txt").write_text("x" * (256 * 1024))
        (work / "key").write_text("test-only")
        env = {**os.environ, "HOME": td, "DEEPSEEK_API_KEY": "test-only"}

        def run(args=(), data=None, expected=0, leak=False):
            command = [BINARY, "--yes", "--no-color", "--key-file", "key",
                       "--endpoint", f"http://127.0.0.1:{server.server_port}/", *args]
            if LEAKS and leak:
                # leaks returns its own status, not the target's exit code.
                if expected:
                    direct = subprocess.run(command, cwd=work, env=env, input=data,
                                            capture_output=True, timeout=90)
                    assert direct.returncode == expected, direct.stderr
                command = ["/usr/bin/leaks", "--noContent", "--atExit", "--", *command]
            p = subprocess.run(command, cwd=work, env=env, input=data,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
            assert p.returncode == (0 if LEAKS and leak else expected), \
                (p.returncode, p.stderr[-2000:])
            if LEAKS and leak:
                reports = re.findall(rb"(\d+) leaks for (\d+) total leaked bytes",
                                     p.stdout + p.stderr)
                assert reports and all(a == b"0" and b == b"0" for a, b in reports), \
                    (p.stdout[-6000:], p.stderr[-2000:])
            return p

        # Repeated successful requests, tool replies, /clear, and session save.
        run(["--session", "loop.json"], ("read\n" * 40 + "/clear\nshort\n/exit\n").encode(),
            leak=True)
        assert len(json.loads((work / "loop.json").read_text())) == 3
        passed.append("repeated-tools-clear-save")

        for mode in ("flood", "large", "large_id", "invalid_id"):
            p = run(["--session", "limit.json"], f"{mode}\n/clear\nshort\n/exit\n".encode())
            assert b"limit" in p.stderr and not (work / "never").exists()
            assert len(json.loads((work / "limit.json").read_text())) == 3
            passed.append(mode + "-recovery")

        history = [{"role": "system", "content": "x" * (7 * MIB - 2000)}]
        session = work / "near.json"
        session.write_text(json.dumps(history))
        p = run(["--session", "near.json", "-p", "batch"], expected=1)
        saved = json.loads(session.read_text())
        assert session.stat().st_size <= 7 * MIB
        assert len(saved) == 5 and saved[-2]["role"] == saved[-1]["role"] == "tool"
        assert json.loads(saved[-2]["content"]) == {"ok": True, "output_omitted": True}
        assert not json.loads(saved[-1]["content"])["ok"] and not (work / "never").exists()
        assert b"conversation reached" in p.stderr
        # Saved protocol can be loaded; an oversized new prompt leaves it intact.
        original = session.read_bytes()
        before = server.requests
        run(["--session", "near.json", "-p", "x" * 4096], expected=1)
        assert session.read_bytes() == original and server.requests == before
        run(["--session", "near.json", "-p", "short"])
        passed.append("bounded-tool-results-and-resume")

        oversized = work / "oversized.json"
        oversized.write_text(json.dumps([{"role": "user", "content": "x" * (7 * MIB)}]))
        original = oversized.read_bytes()
        before = server.requests
        run(["--session", "oversized.json", "-p", "short"], expected=1)
        assert oversized.read_bytes() == original and server.requests == before
        passed.append("oversized-session-preserved")

        for data in (b"x" * (8 * MIB + 1), b"\xff" * (3 * MIB)):
            before = server.requests
            run(data=data + b"\n/clear\nshort\n/exit\n")
            assert server.requests == before + 1
        passed.append("input-and-utf8-expansion-recovery")

        (work / "bad.json").write_text("invalid")
        run(["--session", "bad.json", "-p", "short"], expected=1, leak=True)
        # A regular file as parent resolves during protection but cannot hold
        # the session's temporary file; saving must unwind normally.
        run(["--session", "small.txt/session.json", "-p", "short"], expected=1, leak=True)
        passed.append("session-errors")
        if LEAKS:
            passed.append("macos-leaks-zero-on-success-and-errors")
    print("PASS resources:", ", ".join(passed))
finally:
    server.shutdown()
    server.server_close()
