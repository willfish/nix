#!/usr/bin/env python3
"""Hold idle and sleep only while a Herdr agent is working.

Blocked, idle, done and unknown are safe to suspend. Herdr's socket is the
source of truth: agent.list for the current set, then pane.agent_status_changed
plus pane lifecycle events so a new agent is picked up without waiting for the
reconcile interval. A missing Herdr socket releases the inhibitor.
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
from collections.abc import Callable, Iterator, Mapping, Sequence

BUSY_STATUS = "working"
REFRESH_SECONDS = 30
RETRY_SECONDS = 2
READ_SIZE = 65536

LIFECYCLE_SUBSCRIPTIONS = (
    {"type": "pane.created"},
    {"type": "pane.closed"},
    {"type": "pane.exited"},
    {"type": "pane.agent_detected"},
)


def default_socket_path() -> str:
    override = os.environ.get("HERDR_SOCKET_PATH")
    if override:
        return override
    config_home = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser(
        "~/.config"
    )
    return os.path.join(config_home, "herdr", "herdr.sock")


def inhibit_argv() -> list[str]:
    return [
        "systemd-inhibit",
        "--what=idle:sleep",
        "--mode=block",
        "--who=herdr-agents",
        "--why=Herdr agent working",
        "sleep",
        "infinity",
    ]


def busy(statuses: Mapping[str, str]) -> bool:
    return any(status == BUSY_STATUS for status in statuses.values())


def statuses_from_agents(
    agents: Sequence[Mapping[str, object]],
) -> dict[str, str]:
    statuses: dict[str, str] = {}
    for agent in agents:
        pane_id = agent.get("pane_id")
        status = agent.get("agent_status")
        if (
            isinstance(pane_id, str)
            and pane_id
            and isinstance(status, str)
            and status
        ):
            statuses[pane_id] = status
    return statuses


def subscription_request(
    pane_ids: Sequence[str], request_id: str
) -> dict[str, object]:
    subscriptions: list[dict[str, str]] = [
        dict(item) for item in LIFECYCLE_SUBSCRIPTIONS
    ]
    for pane_id in pane_ids:
        subscriptions.append(
            {"type": "pane.agent_status_changed", "pane_id": pane_id}
        )
    return {
        "id": request_id,
        "method": "events.subscribe",
        "params": {"subscriptions": subscriptions},
    }


def apply_message(
    statuses: dict[str, str], message: Mapping[str, object]
) -> str:
    """Apply one socket line. Return status, refresh, or ignore."""
    if "error" in message:
        return "refresh"

    event = message.get("event")
    data = message.get("data")
    payload = data if isinstance(data, Mapping) else {}
    pane_id = payload.get("pane_id")
    status = payload.get("agent_status")

    if event in ("pane.agent_status_changed", "pane_agent_status_changed"):
        if (
            isinstance(pane_id, str)
            and pane_id
            and isinstance(status, str)
            and status
        ):
            statuses[pane_id] = status
            return "status"
        return "refresh"

    if event in ("pane_closed", "pane_exited"):
        if isinstance(pane_id, str) and pane_id:
            statuses.pop(pane_id, None)
        return "refresh"

    if event in ("pane_created", "pane_agent_detected"):
        return "refresh"

    return "ignore"


class Inhibitor:
    def __init__(
        self,
        argv: Sequence[str],
        popen: Callable[..., subprocess.Popen] = subprocess.Popen,
    ):
        self.argv = list(argv)
        self._popen = popen
        self.proc: subprocess.Popen | None = None

    def set_busy(self, active: bool) -> None:
        if active:
            if self.proc is not None and self.proc.poll() is None:
                return
            self.proc = self._popen(self.argv, start_new_session=True)
            return

        proc = self.proc
        self.proc = None
        if proc is None or proc.poll() is not None:
            return
        _stop_process_group(proc)


def _stop_process_group(proc: subprocess.Popen) -> None:
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        proc.wait(timeout=2)


def _read_line(sock: socket.socket, buffer: bytearray) -> str:
    while b"\n" not in buffer:
        chunk = sock.recv(READ_SIZE)
        if not chunk:
            raise ConnectionError("herdr closed the socket")
        buffer.extend(chunk)
    line, _, rest = buffer.partition(b"\n")
    buffer.clear()
    buffer.extend(rest)
    return line.decode()


def _request(
    path: str, payload: Mapping[str, object], timeout: float
) -> dict[str, object]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(path)
        sock.sendall(json.dumps(payload).encode() + b"\n")
        message = json.loads(_read_line(sock, bytearray()))
    if not isinstance(message, dict):
        raise ConnectionError("herdr returned a non-object response")
    if "error" in message:
        error = message["error"]
        detail = error.get("message") if isinstance(error, Mapping) else error
        raise ConnectionError(f"herdr request failed: {detail}")
    return message


def list_agents(path: str, timeout: float = 5) -> list[Mapping[str, object]]:
    message = _request(
        path,
        {"id": "awake-list", "method": "agent.list", "params": {}},
        timeout,
    )
    result = message.get("result")
    if not isinstance(result, Mapping):
        raise ConnectionError("herdr agent.list returned no result")
    agents = result.get("agents")
    if not isinstance(agents, list):
        raise ConnectionError("herdr agent.list returned no agents")
    return [agent for agent in agents if isinstance(agent, Mapping)]


def iter_subscription(
    path: str,
    pane_ids: Sequence[str],
    timeout: float,
) -> Iterator[dict[str, object]]:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect(path)
        request = subscription_request(pane_ids, "awake-subscribe")
        sock.sendall(json.dumps(request).encode() + b"\n")
        buffer = bytearray()
        started = json.loads(_read_line(sock, buffer))
        if not isinstance(started, dict) or "error" in started:
            raise ConnectionError(f"herdr subscription failed: {started}")
        while True:
            try:
                raw = _read_line(sock, buffer)
            except socket.timeout:
                return
            message = json.loads(raw)
            if isinstance(message, dict):
                yield message
    finally:
        sock.close()


def watch_once(
    path: str,
    statuses: dict[str, str],
    inhibitor: Inhibitor,
    refresh_seconds: float = REFRESH_SECONDS,
) -> None:
    statuses.clear()
    statuses.update(statuses_from_agents(list_agents(path)))
    inhibitor.set_busy(busy(statuses))
    for message in iter_subscription(path, list(statuses), refresh_seconds):
        action = apply_message(statuses, message)
        if action == "status":
            inhibitor.set_busy(busy(statuses))
            continue
        if action == "refresh":
            inhibitor.set_busy(busy(statuses))
            return


def run(
    path: str,
    inhibitor: Inhibitor,
    stop: Callable[[], bool],
    sleep: Callable[[float], None] = time.sleep,
    refresh_seconds: float = REFRESH_SECONDS,
) -> None:
    statuses: dict[str, str] = {}
    while not stop():
        try:
            watch_once(path, statuses, inhibitor, refresh_seconds)
        except (
            FileNotFoundError,
            ConnectionError,
            OSError,
            json.JSONDecodeError,
        ) as exc:
            inhibitor.set_busy(False)
            log(f"herdr unavailable ({exc}); sleep allowed")
            sleep(RETRY_SECONDS)


def log(message: str) -> None:
    print(f"herdr-agent-awake: {message}", file=sys.stderr, flush=True)


def main() -> None:
    stopping = False

    def handle_stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, handle_stop)
    signal.signal(signal.SIGINT, handle_stop)
    inhibitor = Inhibitor(inhibit_argv())
    try:
        run(default_socket_path(), inhibitor, lambda: stopping)
    finally:
        inhibitor.set_busy(False)


if __name__ == "__main__":
    main()
