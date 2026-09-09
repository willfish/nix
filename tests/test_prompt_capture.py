"""Local integration tests. Set MITMDUMP to enable the real proxy checks."""

import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / "home/user/prompt-capture.sh"
MITMDUMP = os.environ.get("MITMDUMP") or shutil.which("mitmdump")


class AddonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "capture.jsonl"
        env = patch.dict(os.environ, PROMPT_CAPTURE_JSONL=str(self.log),
                         PROMPT_CAPTURE_RESPONSE_BODY="1")
        env.start()
        self.addCleanup(env.stop)
        addon = SOURCE.read_text().split("<<'PYEOF'\n", 1)[1]
        addon = addon.split("\nPYEOF", 1)[0]
        self.namespace = {}
        exec(compile(addon, "capture-addon", "exec"), self.namespace)
        self.addCleanup(self.namespace["_fh"].close)
        self.flow = SimpleNamespace(
            request=SimpleNamespace(
                method="GET", pretty_host="example.test", path="/events"
            ),
            response=SimpleNamespace(
                headers={"content-type": "text/event-stream"},
                status_code=200, stream=False
            ),
        )

    def test_stream_capture_preserves_split_utf8_and_original_bytes(self):
        self.namespace["addons"][0].responseheaders(self.flow)
        parts = [b"data: \xe2", b"\x82\xac\n\n", b""]
        for part in parts:
            self.assertEqual(self.flow.response.stream(part), part)
        records = [json.loads(line)
                   for line in self.log.read_text().splitlines()]
        captured = "".join(r["response_body"] for r in records)
        self.assertEqual(captured, "data: €\n\n")

    def test_streaming_without_body_capture_does_not_log_response_content(self):
        with patch.dict(os.environ, PROMPT_CAPTURE_RESPONSE_BODY="0"):
            self.namespace["addons"][0].responseheaders(self.flow)
        self.assertIs(self.flow.response.stream, True)
        self.assertEqual(self.log.read_text(), "")


@unittest.skipUnless(MITMDUMP, "set MITMDUMP to run proxy integration tests")
class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="prompt-capture-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.script = self.root / "capture.sh"
        self.script.write_text(SOURCE.read_text().replace(
            "codex) port=8301", f"codex) port={self.port}"
        ))
        self.env = dict(os.environ, MITMDUMP=MITMDUMP,
                        FLOCK=os.environ.get("FLOCK") or shutil.which("flock"),
                        XDG_STATE_HOME=str(self.root),
                        PROMPT_CAPTURE_RESPONSE_BODY="1")
        self.cdir = self.root / "prompt-capture"

    def stop(self, proc):
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate(timeout=5)

    def start(self):
        ready = self.root / "ready"
        proc = subprocess.Popen(
            ["bash", str(self.script), "codex", "--", sys.executable, "-c",
             "import pathlib,sys,time; "
             "pathlib.Path(sys.argv[1]).touch(); time.sleep(60)",
             str(ready)], env=self.env, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, start_new_session=True,
            umask=0o022,
        )
        self.addCleanup(self.stop, proc)
        deadline = time.monotonic() + 20
        while not ready.exists() and time.monotonic() < deadline:
            if proc.poll() is not None:
                self.fail(proc.communicate()[1])
            time.sleep(0.05)
        self.assertTrue(ready.exists(), "wrapped command did not start")
        return proc

    def test_sse_arrives_before_upstream_finishes_and_is_captured(self):
        self.start()
        release = threading.Event()
        received = threading.Event()
        errors = []
        body = b"data: first\n\ndata: second\n\n"

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.send_header(
                    "Content-Type", "text/event-stream; charset=utf-8"
                )
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(b"data: first\n\n")
                self.wfile.flush()
                release.wait(5)
                self.wfile.write(b"data: second\n\n")
                self.wfile.flush()

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()

        def read_response():
            conn = http.client.HTTPConnection(
                "127.0.0.1", self.port, timeout=10
            )
            try:
                url = f"http://127.0.0.1:{server.server_port}/events"
                conn.request("GET", url)
                resp = conn.getresponse()
                first = resp.read(13)
                self.assertEqual(first, b"data: first\n\n")
                received.set()
                self.assertEqual(first + resp.read(), body)
            except Exception as exc:
                errors.append(exc)
            finally:
                conn.close()

        reader = threading.Thread(target=read_response, daemon=True)
        reader.start()
        try:
            self.assertTrue(
                received.wait(2), "proxy buffered the first SSE event"
            )
        finally:
            release.set()
            reader.join(10)
            server.shutdown()
            server.server_close()
            worker.join(2)
        self.assertFalse(errors, errors)
        records = [json.loads(line) for line in
                   (self.cdir / "codex.jsonl").read_text().splitlines()]
        captured = "".join(r["response_body"] for r in records
                           if r["kind"] == "response_chunk")
        self.assertEqual(captured, body.decode())

    def test_overlapping_capture_leaves_the_original_proxy_running(self):
        first = self.start()
        pidfile = self.cdir / "codex.pid"
        original_pid = pidfile.read_text()
        second = subprocess.run(
            ["bash", str(self.script), "codex", "--", "true"],
            env=self.env, capture_output=True, text=True, timeout=20,
        )
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("already active", second.stderr)
        self.assertEqual(pidfile.read_text(), original_pid)
        os.kill(int(original_pid), 0)
        self.assertIsNone(first.poll())
        self.stop(first)
        (self.root / "ready").unlink()
        self.start()  # Normal cleanup releases the lock for the next run.

    def test_private_files_and_certificate_only_trust_bundle(self):
        # Repair existing files when upgrading from the old helper.
        self.cdir.mkdir(mode=0o755)
        for name in ("codex.jsonl", "codex-ca.pem", "codex-server.log"):
            path = self.cdir / name
            path.touch(mode=0o644)
        self.start()
        self.assertEqual(self.cdir.stat().st_mode & 0o777, 0o700)
        names = ("codex.jsonl", "codex-ca.pem", "codex-server.log", "codex.pid")
        for name in names:
            mode = (self.cdir / name).stat().st_mode & 0o777
            self.assertEqual(mode, 0o600, name)
        bundle = (self.cdir / "codex-ca.pem").read_text()
        self.assertIn("BEGIN CERTIFICATE", bundle)
        self.assertNotIn("PRIVATE KEY", bundle)

    def test_busy_port_does_not_launch_the_wrapped_command(self):
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", self.port))
            occupied.listen()
            result = subprocess.run(
                ["bash", str(self.script), "codex", "--", "touch",
                 str(self.root / "ran")],
                env=self.env, capture_output=True, text=True, timeout=15,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "ran").exists())

    def test_stop_cleans_up_a_stranded_proxy_and_allows_another_capture(self):
        first = self.start()
        first.kill()  # Leave the CLI and proxy alive, as after a wrapper crash.
        first.wait(timeout=5)
        blocked = subprocess.run(
            ["bash", str(self.script), "codex", "--", "true"],
            env=self.env, capture_output=True, text=True, timeout=10,
        )
        self.assertNotEqual(blocked.returncode, 0)
        stopped = subprocess.run(
            ["bash", str(self.script), "stop", "codex"],
            env=self.env, capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        (self.root / "ready").unlink()
        self.start()


if __name__ == "__main__":
    unittest.main()
