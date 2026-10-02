"""Herdr working state is the only reason to hold idle and sleep."""

import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "home/config/hyprland/agent-awake.c"


def compile_agent(directory):
    binary = Path(directory) / "herdr-agent-awake"
    subprocess.run(
        [
            "cc",
            "-std=c17",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-O2",
            "-o",
            str(binary),
            str(SOURCE),
        ],
        check=True,
    )
    return binary


class BuildTests(unittest.TestCase):
    def test_self_test_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = compile_agent(directory)
            completed = subprocess.run(
                [str(binary), "--self-test"],
                text=True,
                capture_output=True,
                timeout=10,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_session_menu_suspend_overrides_the_inhibitor(self):
        script = (ROOT / "home/config/hyprland/session.sh").read_text()
        self.assertIn("systemctl suspend --ignore-inhibitors", script)
        self.assertNotIn("systemctl reboot --ignore-inhibitors", script)


class SocketTests(unittest.TestCase):
    def test_missing_socket_fails_open(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = compile_agent(directory)
            missing = str(Path(directory) / "missing.sock")
            proc = subprocess.Popen(
                [str(binary)],
                env={
                    **os.environ,
                    "HERDR_SOCKET_PATH": missing,
                    "PATH": "/usr/bin:/bin",
                },
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 3
                err = ""
                while (
                    time.monotonic() < deadline
                    and "sleep allowed" not in err
                ):
                    chunk = proc.stderr.readline()
                    if not chunk:
                        break
                    err += chunk
                self.assertIn("sleep allowed", err)
                self.assertIsNone(proc.poll())
            finally:
                os.killpg(proc.pid, 15)
                proc.wait(timeout=3)
                proc.stderr.close()

    def _readline(self, conn):
        raw = b""
        conn.settimeout(5)
        while b"\n" not in raw:
            chunk = conn.recv(65536)
            if not chunk:
                break
            raw += chunk
        return raw.partition(b"\n")[0].decode()

    def test_working_agent_inhibits_until_it_stops(self):
        sleep = shutil.which("sleep")
        self.assertIsNotNone(sleep)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            binary = compile_agent(directory)
            log = directory / "inhibit.log"
            bindir = directory / "bin"
            bindir.mkdir()
            inhibit = bindir / "systemd-inhibit"
            inhibit.write_text(
                "#!/bin/sh\n"
                'printf \'start %s\\n\' "$*" >> "$HERDR_INHIBIT_LOG"\n'
                'echo $$ >> "$HERDR_INHIBIT_LOG"\n'
                f"exec {sleep} infinity\n"
            )
            inhibit.chmod(0o755)
            path = str(directory / "herdr.sock")
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(path)
            server.listen(4)
            server.settimeout(5)
            proc = subprocess.Popen(
                [str(binary)],
                env={
                    **os.environ,
                    "HERDR_SOCKET_PATH": path,
                    "HERDR_INHIBIT_LOG": str(log),
                    "PATH": f"{bindir}:{os.environ.get('PATH', '')}",
                },
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            try:
                conn, _ = server.accept()
                line = self._readline(conn)
                self.assertIn("agent.list", line)
                conn.sendall(
                    b'{"id":"awake-list","result":{"agents":['
                    b'{"pane_id":"w1:p1","agent_status":"working"}]}}\n'
                )
                deadline = time.monotonic() + 5
                text = ""
                while time.monotonic() < deadline and "start " not in text:
                    time.sleep(0.05)
                    text = log.read_text() if log.exists() else ""
                self.assertIn("--what=idle:sleep", text)
                self.assertIn("--who=herdr-agents", text)
                pid = int(text.strip().splitlines()[-1])
                self.assertTrue(Path(f"/proc/{pid}").exists())
                conn.close()

                conn, _ = server.accept()
                subscribe = self._readline(conn)
                self.assertIn("events.subscribe", subscribe)
                self.assertIn("pane.created", subscribe)
                self.assertIn("pane.closed", subscribe)
                self.assertIn("pane.exited", subscribe)
                self.assertIn("pane.agent_detected", subscribe)
                self.assertIn("w1:p1", subscribe)
                ack = (
                    b'{"id":"awake-subscribe","result":'
                    b'{"type":"subscription_started"}}\n'
                )
                blocked = (
                    b'{"event":"pane.agent_status_changed","data":'
                    b'{"pane_id":"w1:p1","agent_status":"blocked"}}\n'
                )
                closed = b'{"event":"pane_closed","data":{"pane_id":"w1:p1"}}\n'
                conn.sendall(ack + blocked + closed)
                deadline = time.monotonic() + 5
                alive = Path(f"/proc/{pid}").exists()
                while time.monotonic() < deadline and alive:
                    time.sleep(0.05)
                    alive = Path(f"/proc/{pid}").exists()
                self.assertFalse(
                    Path(f"/proc/{pid}").exists(),
                    "inhibitor still running",
                )
                conn.close()

                conn, _ = server.accept()
                again = self._readline(conn)
                self.assertIn("agent.list", again)
                conn.sendall(b'{"id":"awake-list","result":{"agents":[]}}\n')
                conn.close()
            finally:
                os.killpg(proc.pid, 15)
                proc.wait(timeout=3)
                proc.stderr.close()
                server.close()


if __name__ == "__main__":
    unittest.main()
