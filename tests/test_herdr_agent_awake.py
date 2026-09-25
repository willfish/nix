"""Herdr working state is the only reason to hold idle and sleep."""

import importlib.util
import json
import socket
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "agent_awake", ROOT / "home/config/hyprland/agent_awake.py"
)
awake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(awake)


class StatusTests(unittest.TestCase):
    def test_only_working_holds_the_machine(self):
        self.assertFalse(
            awake.busy(
                {
                    "blocked": "blocked",
                    "idle": "idle",
                    "done": "done",
                    "unknown": "unknown",
                }
            )
        )
        self.assertTrue(awake.busy({"one": "idle", "two": "working"}))

    def test_list_replaces_stale_panes(self):
        statuses = awake.statuses_from_agents(
            [
                {"pane_id": "w1:p1", "agent": "pi", "agent_status": "blocked"},
                {"pane_id": "w2:p1", "agent": "codex", "agent_status": "done"},
                {"agent_status": "working"},
                {"pane_id": "w3:p1"},
            ]
        )
        self.assertEqual(statuses, {"w1:p1": "blocked", "w2:p1": "done"})
        self.assertFalse(awake.busy(statuses))

    def test_status_event_updates_without_refresh(self):
        statuses = {"w1:p1": "idle"}
        action = awake.apply_message(
            statuses,
            {
                "event": "pane.agent_status_changed",
                "data": {
                    "pane_id": "w1:p1",
                    "agent_status": "working",
                    "agent": "pi",
                },
            },
        )
        self.assertEqual(action, "status")
        self.assertTrue(awake.busy(statuses))

    def test_generic_status_event_is_accepted(self):
        statuses = {}
        action = awake.apply_message(
            statuses,
            {
                "event": "pane_agent_status_changed",
                "data": {
                    "type": "pane_agent_status_changed",
                    "pane_id": "w9:p1",
                    "agent_status": "working",
                },
            },
        )
        self.assertEqual(action, "status")
        self.assertEqual(statuses["w9:p1"], "working")

    def test_closed_pane_is_removed_and_refreshed(self):
        statuses = {"w1:p1": "working"}
        action = awake.apply_message(
            statuses,
            {
                "event": "pane_closed",
                "data": {"type": "pane_closed", "pane_id": "w1:p1"},
            },
        )
        self.assertEqual(action, "refresh")
        self.assertFalse(awake.busy(statuses))

    def test_new_pane_requests_refresh(self):
        self.assertEqual(
            awake.apply_message(
                {}, {"event": "pane_agent_detected", "data": {}}
            ),
            "refresh",
        )
        self.assertEqual(
            awake.apply_message({}, {"event": "pane_created", "data": {}}),
            "refresh",
        )

    def test_subscription_covers_lifecycle_and_known_panes(self):
        request = awake.subscription_request(
            ["w1:p1", "w2:p1"], "awake-subscribe"
        )
        types = [item["type"] for item in request["params"]["subscriptions"]]
        self.assertEqual(
            types,
            [
                "pane.created",
                "pane.closed",
                "pane.exited",
                "pane.agent_detected",
                "pane.agent_status_changed",
                "pane.agent_status_changed",
            ],
        )
        self.assertEqual(
            request["params"]["subscriptions"][-1],
            {"type": "pane.agent_status_changed", "pane_id": "w2:p1"},
        )

    def test_session_menu_suspend_overrides_the_inhibitor(self):
        script = (ROOT / "home/config/hyprland/session.sh").read_text()
        self.assertIn("systemctl suspend --ignore-inhibitors", script)
        self.assertNotIn("systemctl reboot --ignore-inhibitors", script)


class InhibitorTests(unittest.TestCase):
    def test_busy_starts_once_and_release_stops_the_group(self):
        started = []
        stopped = []

        class Proc:
            pid = 42

            def poll(self):
                return None

            def wait(self, timeout=None):
                return 0

        def popen(argv, start_new_session):
            started.append((argv, start_new_session))
            return Proc()

        original = awake.os.killpg
        awake.os.killpg = lambda pid, sig: stopped.append((pid, sig))
        try:
            inhibitor = awake.Inhibitor(["sleep", "infinity"], popen=popen)
            inhibitor.set_busy(True)
            inhibitor.set_busy(True)
            inhibitor.set_busy(False)
        finally:
            awake.os.killpg = original

        self.assertEqual(len(started), 1)
        self.assertTrue(started[0][1])
        self.assertEqual(stopped, [(42, awake.signal.SIGTERM)])
        self.assertIsNone(inhibitor.proc)


class SocketTests(unittest.TestCase):
    def test_watch_follows_status_then_refreshes_on_close(self):
        path = self._serve()
        started = []

        class Proc:
            pid = 7

            def poll(self):
                return None

            def wait(self, timeout=None):
                return 0

        def popen(argv, start_new_session):
            started.append(argv)
            return Proc()

        inhibitor = awake.Inhibitor(["inhibit"], popen=popen)
        awake.os.killpg = lambda pid, sig: started.append(("stop", pid, sig))
        try:
            awake.watch_once(path, {}, inhibitor, refresh_seconds=2)
        finally:
            awake.os.killpg = os_killpg

        self.assertEqual(started[0], ["inhibit"])
        self.assertEqual(started[-1][0], "stop")

    def _serve(self):
        import tempfile

        self._tmp = tempfile.TemporaryDirectory()
        path = str(Path(self._tmp.name) / "herdr.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        server.listen(2)
        self.addCleanup(server.close)
        self.addCleanup(self._tmp.cleanup)

        def accept_one():
            conn, _ = server.accept()
            with conn:
                raw = b""
                while b"\n" not in raw:
                    raw += conn.recv(65536)
                request = json.loads(raw.split(b"\n", 1)[0])
                if request["method"] == "agent.list":
                    body = {
                        "id": request["id"],
                        "result": {
                            "type": "agent_list",
                            "agents": [
                                {
                                    "pane_id": "w1:p1",
                                    "agent": "pi",
                                    "agent_status": "working",
                                }
                            ],
                        },
                    }
                    conn.sendall(json.dumps(body).encode() + b"\n")
                    return
                conn.sendall(
                    json.dumps(
                        {
                            "id": request["id"],
                            "result": {"type": "subscription_started"},
                        }
                    ).encode()
                    + b"\n"
                )
                conn.sendall(
                    json.dumps(
                        {
                            "event": "pane.agent_status_changed",
                            "data": {
                                "pane_id": "w1:p1",
                                "agent_status": "blocked",
                            },
                        }
                    ).encode()
                    + b"\n"
                )
                conn.sendall(
                    json.dumps(
                        {
                            "event": "pane_closed",
                            "data": {"type": "pane_closed", "pane_id": "w1:p1"},
                        }
                    ).encode()
                    + b"\n"
                )

        threading.Thread(target=accept_one, daemon=True).start()
        threading.Thread(target=accept_one, daemon=True).start()
        return path


os_killpg = awake.os.killpg


if __name__ == "__main__":
    unittest.main()
