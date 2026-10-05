"""Real native daemon journey using synthetic PCM, loopback STT and a fake Pi.

The fixture cannot reach user services, devices, credentials or real panes.
"""

import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid


BINARY = str(Path(sys.argv[1]).resolve())


def call(path, request):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(4)
        connection.connect(str(path))
        connection.sendall(json.dumps(request).encode() + b"\n")
        with connection.makefile("rb") as incoming:
            return json.loads(incoming.readline(1024 * 1024))


def until(predicate, label, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.04)
    raise AssertionError(f"Native journey timed out: {label}")


# Nix's TMPDIR can be long; the managed adapter name must fit sockaddr_un.
with tempfile.TemporaryDirectory(prefix="vj-", dir="/tmp") as directory:
    root = Path(directory)
    runtime = root / "run"
    runtime.mkdir(mode=0o700)
    private = runtime / "pi-voice"
    private.mkdir(mode=0o700)
    tools = root / "bin"
    tools.mkdir()
    calls = root / "calls.jsonl"
    pid, bridge = os.getpid(), str(uuid.uuid4())
    pane, session = "w1:p1", "native-fixture-session"
    adapter_path = private / f"pi-{pid}-{bridge}.sock"
    herdr_path = root / "herdr.sock"
    control = private / "control.sock"
    target = {
        "pane": pane,
        "socket": str(herdr_path),
        "pid": pid,
        "session": session,
        "bridge_id": bridge,
        "activation": 1,
        "adapter_socket": str(adapter_path),
        "harness": "pi",
        "team_child": False,
        "model": "synthetic-model",
        "thinking": "medium",
    }
    identity = {
        key: target[key]
        for key in ("pid", "session", "bridge_id", "activation", "harness")
    }
    state = {
        "token": None,
        "staged": [],
        "submitted": [],
        "armed": False,
        "text": "",
    }
    guard = threading.Lock()
    finished = threading.Event()
    stt_bodies = []

    class Speech(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            payload = b'{"status":"ok"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            stt_bodies.append(body)
            if len(stt_bodies) == 1:
                # Let three more chunks accumulate while transcription is busy.
                time.sleep(4.2)
            payload = json.dumps(
                {"text": f"Native phrase {len(stt_bodies)}."}
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, *_args):
            pass

    http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Speech)
    http_thread = threading.Thread(target=http.serve_forever, daemon=True)
    http_thread.start()
    adapter = socket.socket(socket.AF_UNIX)
    adapter.bind(str(adapter_path))
    adapter_path.chmod(0o600)
    adapter.listen()
    adapter.settimeout(0.1)
    herdr = socket.socket(socket.AF_UNIX)
    herdr.bind(str(herdr_path))
    herdr_path.chmod(0o600)
    herdr.listen()

    def serve_pi():
        while not finished.is_set():
            try:
                connection, _ = adapter.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(3)
                try:
                    with connection.makefile("rb") as incoming:
                        request = json.loads(incoming.readline(256 * 1024))
                    with guard:
                        valid = request.get("token") == state["token"] and all(
                            request.get(key) == value
                            for key, value in identity.items()
                        )
                        command = request.get("command")
                        if not valid:
                            response = {
                                "ok": False,
                                "error": "fixture identity mismatch",
                            }
                        elif command == "stage":
                            state["text"] += (
                                "\n" if state["text"] else ""
                            ) + request["text"]
                            state["staged"].append(request["text"])
                            state["armed"] = True
                            response = {"ok": True, "result": {}}
                        elif command == "submit" and state["armed"]:
                            state["submitted"].append(state["text"])
                            state["armed"] = False
                            response = {"ok": True, "result": {}}
                        elif command == "status":
                            response = {
                                "ok": True,
                                "result": {
                                    **identity,
                                    "ready": True,
                                    "accepts_input": True,
                                    "state": "idle",
                                    "draft": state["armed"],
                                    "draft_state": "staged"
                                    if state["armed"]
                                    else "none",
                                },
                            }
                        else:
                            response = {"ok": False, "error": "no owned draft"}
                    connection.sendall(json.dumps(response).encode() + b"\n")
                except (OSError, ValueError):
                    pass

    adapter_thread = threading.Thread(target=serve_pi, daemon=True)
    adapter_thread.start()
    dump = [
        {
            "type": "PipeWire:Interface:Node",
            "info": {
                "props": {
                    "node.name": "synthetic",
                    "node.description": "Synthetic microphone",
                    "media.class": "Audio/Source",
                },
                "params": {"Props": [{"mute": False}]},
            },
        },
        {
            "props": {"metadata.name": "default"},
            "metadata": [
                {
                    "key": "default.audio.source",
                    "value": {"name": "synthetic"},
                },
            ],
        },
    ]
    herdr_result = {
        "ok": True,
        "result": {
            "process_info": {
                "pane_id": pane,
                "foreground_processes": [{"pid": pid, "name": "pi"}],
            },
            "snapshot": {
                "workspaces": [
                    {"workspace_id": "w1", "name": "Native fixture"}
                ],
                "tabs": [{"tab_id": "t1", "name": "Synthetic pane"}],
                "panes": [
                    {"pane_id": pane, "workspace_id": "w1", "tab_id": "t1"}
                ],
            },
        },
    }
    script = (
        f"#!{sys.executable}\n"
        "import array, json, math, os, signal, sys, time\n"
        f"with open({str(calls)!r}, 'a') as log:\n"
        "    log.write(json.dumps(sys.argv) + '\\n')\n"
        "name = os.path.basename(sys.argv[0])\n"
        "if name == 'systemctl':\n"
        "    if 'show' in sys.argv: print('inactive')\n"
        f"elif name == 'pw-dump': print({json.dumps(dump)!r})\n"
        f"elif name == 'herdr': print({json.dumps(herdr_result)!r})\n"
        "elif name == 'pw-record':\n"
        "    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))\n"
        "    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
        "    for frame in range(500):\n"
        "        voiced = any(start <= frame < start + 20\n"
        "                     for start in (0, 60, 120, 180))\n"
        "        pcm = array.array('h', [int(1500*math.sin(i*0.17))\n"
        "                      if voiced else 0 for i in range(320)])\n"
        "        try: os.write(1, pcm.tobytes())\n"
        "        except BrokenPipeError: break\n"
        "        time.sleep(0.02)\n"
        "elif name == 'pw-play': sys.stdin.buffer.read()\n"
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
        path = tools / name
        path.write_text(script)
        path.chmod(0o700)
    config = root / "config.json"
    base = f"http://127.0.0.1:{http.server_port}"
    config.write_text(
        json.dumps(
            {
                "stt_url": base + "/inference",
                "stt_health_url": base + "/health",
                "tts_url": base + "/speech",
                "tts_health_url": base + "/models",
                "tts_enabled": False,
                "auto_speak": False,
                "voice_preferences_path": str(root / "voice-mode"),
            }
        )
    )
    env = {
        "HOME": str(root),
        "XDG_RUNTIME_DIR": str(runtime),
        "PI_VOICE_CONFIG": str(config),
        "PATH": str(tools),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
    }
    log_path = root / "daemon.log"
    log = log_path.open("wb")
    child = subprocess.Popen(
        [BINARY, "serve"], env=env, stdout=log, stderr=log
    )
    try:

        def ready():
            assert child.poll() is None, log_path.read_text()
            try:
                return call(control, {"action": "status"}).get("ok")
            except OSError:
                return False

        until(ready, "controller startup")
        attached = call(control, {"action": "attach", "target": target})
        assert attached.get("ok") and attached["attached"].get("token"), (
            attached
        )
        state["token"] = attached["attached"]["token"]
        event = {
            **identity,
            "type": "ready",
            "ready": True,
            "accepts_input": True,
            "state": "idle",
            "adapter_socket": str(adapter_path),
            "draft_state": "none",
        }
        assert call(
            control,
            {
                "action": "harness-event",
                "token": state["token"],
                "event": event,
            },
        )["accepted"]
        status = call(control, {"action": "status"})
        assert (
            status["pane"] == pane and status["connection_state"] == "ready"
        ), status
        rejected = call(
            control,
            {
                "action": "harness-event",
                "token": state["token"],
                "event": {**event, "session": "foreign"},
            },
        )
        assert rejected["accepted"] is False
        started = call(control, {"action": "record"})
        assert started["ok"], started
        until(
            lambda: len(state["staged"]) >= 4,
            "four queued capture slices through slow STT and Pi stage",
            seconds=20,
        )
        assert state["submitted"] == [], (
            "recording submitted without explicit Send"
        )
        stopped = call(control, {"action": "interact"})
        assert stopped["ok"], stopped
        until(
            lambda: call(control, {"action": "status"})["phase"] == "draft",
            "recording stopped",
        )
        expected = [f"Native phrase {n}." for n in range(1, 5)]
        assert len(stt_bodies) == 4 and all(
            b"RIFF" in body for body in stt_bodies
        )
        assert state["staged"] == expected, state["staged"]
        microphone = call(control, {"action": "status"})["microphone"]
        assert (
            microphone["name"] == "Synthetic microphone"
            and microphone["target"] is None
        ), microphone
        assert (
            microphone["muted"] is False and microphone["missing"] is False
        ), microphone
        submitted = call(control, {"action": "send"})
        assert submitted["ok"], submitted
        assert state["submitted"] == ["\n".join(expected)], state["submitted"]
        call(control, {"action": "send"})
        assert len(state["submitted"]) == 1, (
            "duplicate explicit Send replayed the draft"
        )
        detached = call(
            control, {"action": "detach", "token": state["token"], **identity}
        )
        assert detached["accepted"] is True
        assert call(control, {"action": "status"})["sessions"] == []
        child.terminate()
        assert child.wait(timeout=8) == 0, log_path.read_text()
        print(
            "Native journey: managed Pi attach and identity guard; "
            "synthetic PCM through STT; staged draft; "
            "one explicit submit; detach and shutdown"
        )
    except BaseException:
        print(log_path.read_text(), file=sys.stderr)
        raise
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=3)
        log.close()
        finished.set()
        adapter_thread.join(timeout=4)
        adapter.close()
        herdr.close()
        http.shutdown()
        http.server_close()
        http_thread.join(timeout=3)
