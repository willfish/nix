"""Exercise the production native daemon with private sockets and fake devices.

No microphone, user service, cloud key, or live Pi session is available here.
"""

import concurrent.futures
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time


BINARY = str(Path(sys.argv[1]).resolve())
MAX_FRAME = 1024 * 1024


def request(path, value, timeout=3):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall(json.dumps(value).encode() + b"\n")
        with client.makefile("rb") as stream:
            raw = stream.readline(MAX_FRAME + 1)
    assert raw.endswith(b"\n") and len(raw) <= MAX_FRAME, (
        "invalid response frame"
    )
    return json.loads(raw)


with tempfile.TemporaryDirectory(prefix="voice-daemon-") as temporary:
    root = Path(temporary)
    runtime = root / "run"
    runtime.mkdir(mode=0o700)
    tools = root / "bin"
    tools.mkdir()
    calls = root / "calls.jsonl"
    script = (
        f"#!{sys.executable}\n"
        "import json, os, sys\n"
        f"with open({str(calls)!r}, 'a') as log:\n"
        "    log.write(json.dumps(sys.argv) + '\\n')\n"
        "name = os.path.basename(sys.argv[0])\n"
        "if name == 'systemctl':\n"
        "    if 'show' in sys.argv: print('inactive')\n"
        "elif name == 'pw-dump': print('[]')\n"
        "else: sys.exit(97)\n"
    )
    for name in (
        "systemctl",
        "pw-dump",
        "pw-record",
        "pw-play",
        "herdr",
        "wl-copy",
    ):
        tool = tools / name
        tool.write_text(script)
        tool.chmod(0o700)
    config = root / "config.json"
    config.write_text(
        json.dumps(
            {
                "stt_url": "http://127.0.0.1:9/inference",
                "stt_health_url": "http://127.0.0.1:9/health",
                "tts_url": "http://127.0.0.1:9/speech",
                "tts_health_url": "http://127.0.0.1:9/models",
                "tts_enabled": False,
                "auto_speak": False,
                "voice_preferences_path": str(root / "voice-mode"),
            }
        )
    )
    # Do not inherit credentials, service sockets or the real user's home.
    env = {
        "HOME": str(root),
        "XDG_RUNTIME_DIR": str(runtime),
        "PI_VOICE_CONFIG": str(config),
        "PATH": str(tools),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
    }
    path = runtime / "pi-voice/control.sock"
    log_path = root / "daemon.log"
    log = log_path.open("wb")
    child = None

    def start():
        process = subprocess.Popen(
            [BINARY, "serve"], env=env, stdout=log, stderr=log
        )
        try:
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                assert process.poll() is None, log_path.read_text()
                try:
                    if request(path, {"action": "status"}, timeout=0.5).get(
                        "ok"
                    ):
                        return process
                except (OSError, ValueError):
                    time.sleep(0.05)
            raise AssertionError("native daemon did not become ready")
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            raise

    def stop(process):
        process.terminate()
        try:
            result = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
            raise AssertionError(
                "native daemon did not stop with an idle client"
            ) from None
        assert result == 0, (
            f"native daemon exit {result}: {log_path.read_text()}"
        )
        assert not path.exists(), "native daemon left its control socket"

    try:
        help_result = subprocess.run(
            [BINARY, "--help"], env=env, capture_output=True, timeout=5
        )
        assert (
            help_result.returncode == 0 and b"pi-voice" in help_result.stdout
        )
        child = start()
        assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
        assert stat.S_IMODE(path.stat().st_mode) == 0o600
        status = request(path, {"action": "status"})
        assert (
            status["phase"] == "idle"
            and status["connection_state"] == "unselected"
        )
        assert status["sessions"] == [] and not status["speech_available"]
        for key in (
            "draft",
            "pending",
            "retained",
            "retry",
            "recording",
            "transcribing",
            "speaking",
        ):
            assert type(status[key]) is bool, (key, status[key])
        for key in ("record", "send", "read", "unknown-command"):
            assert request(path, {"action": key})["ok"] is False, key
        assert request(path, {"action": "fixture-state"})["ok"] is False, (
            "test command exposed by daemon"
        )
        assert request(path, {"action": "team-toggle"})["show_team"] is True
        assert (
            stat.S_IMODE((path.parent / "selection.json").stat().st_mode)
            == 0o600
        )

        # A second daemon must not steal or delete the live socket.
        second = subprocess.Popen(
            [BINARY, "serve"], env=env, stdout=log, stderr=log
        )
        try:
            assert second.wait(timeout=5) != 0, (
                "second daemon accepted the same runtime"
            )
        except subprocess.TimeoutExpired:
            second.kill()
            second.wait(timeout=3)
            raise AssertionError(
                "second daemon stole or blocked the live socket"
            ) from None
        assert request(path, {"action": "status"})["ok"]

        # One incomplete request must not block other callers or the monitor.
        with socket.socket(socket.AF_UNIX) as idle:
            idle.connect(str(path))
            idle.sendall(b'{"action":')
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                futures = [
                    pool.submit(request, path, {"action": "status"})
                    for _ in range(24)
                ]
                assert all(
                    future.result(timeout=5)["ok"] for future in futures
                )
            stop(child)
            child = None

        child = start()
        assert request(path, {"action": "status"})["show_team"] is True
        # A client disappearing before its response must never raise SIGPIPE.
        for _ in range(32):
            with socket.socket(socket.AF_UNIX) as gone:
                gone.connect(str(path))
                gone.sendall(b'{"action":"status"}\n')
        # A bounded server may reject excess clients while the burst drains.
        # Retry only this read-only status probe; mutations are never replayed.
        deadline = time.monotonic() + 3
        while True:
            assert child.poll() is None, log_path.read_text()
            try:
                assert request(path, {"action": "status"})["ok"]
                break
            except (OSError, ValueError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)
        stop(child)
        child = None
        observed = [
            json.loads(line) for line in calls.read_text().splitlines()
        ]
        assert all(
            Path(row[0]).name in ("systemctl", "pw-dump") for row in observed
        ), observed
        # Run the real launcher and CLI against a disposable control socket.
        # A lost mutation acknowledgement must never cause an automatic replay.
        received = []
        server_stop = threading.Event()
        failures = []
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(str(path))
            listener.listen(8)
            listener.settimeout(0.1)

            def serve_cli():
                try:
                    while not server_stop.is_set():
                        try:
                            connection, _ = listener.accept()
                        except socket.timeout:
                            continue
                        with connection:
                            connection.settimeout(2)
                            with connection.makefile("rb") as incoming:
                                command = json.loads(
                                    incoming.readline(MAX_FRAME)
                                )
                            received.append(command)
                            if command["action"] != "interact":
                                connection.sendall(b'{"ok":true}\n')
                except BaseException as error:
                    failures.append(error)

            server = threading.Thread(target=serve_cli)
            server.start()
            try:
                uncertain = subprocess.run(
                    [BINARY, "interact"],
                    env=env,
                    capture_output=True,
                    timeout=5,
                )
                assert (
                    uncertain.returncode != 0
                    and b"unknown" in uncertain.stderr
                ), uncertain.stderr
                assert [row["action"] for row in received] == ["interact"], (
                    received
                )
                received.clear()
                extension = root / ".pi/agent/extensions/pi-voice.ts"
                extension.parent.mkdir(parents=True)
                extension.write_text(
                    "// Installation marker for the disposable launcher test.\n"
                )
                pi_result = root / "pi-launch.json"
                pi = tools / "pi"
                pi.write_text(
                    f"#!{sys.executable}\nimport json, os, sys\n"
                    f"with open({str(pi_result)!r}, 'w') as out:\n"
                    "    json.dump({'args': sys.argv[1:], "
                    "'token': os.environ['AGENT_VOICE_TOKEN'], "
                    "'kind': os.environ['AGENT_VOICE_KIND']}, out)\n"
                )
                pi.chmod(0o700)
                launcher_env = dict(
                    env,
                    HERDR_ENV="1",
                    HERDR_PANE_ID="disposable-pane",
                    HERDR_SOCKET_PATH=str(root / "fake-herdr.sock"),
                )
                launched = subprocess.run(
                    [BINARY, "--", "--native-smoke"],
                    env=launcher_env,
                    capture_output=True,
                    timeout=10,
                )
                assert launched.returncode == 0, launched.stderr.decode()
                assert [row["action"] for row in received] == [
                    "status",
                    "warm",
                    "register",
                    "unregister",
                ], received
                launch = json.loads(pi_result.read_text())
                assert (
                    launch["args"] == ["--native-smoke"]
                    and launch["kind"] == "pi"
                ), launch
                assert (
                    received[2]["token"]
                    == launch["token"]
                    == received[3]["token"]
                )
                assert not (path.parent / launch["token"]).exists(), (
                    "launcher left its runtime directory"
                )
            finally:
                server_stop.set()
                server.join(timeout=3)
                assert not server.is_alive() and not failures, failures
        path.unlink()
        print(
            "Native daemon smoke: private socket, persistence, "
            "concurrent clients, safe shutdown, real launcher "
            "and no mutation replay"
        )
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=3)
        log.close()
