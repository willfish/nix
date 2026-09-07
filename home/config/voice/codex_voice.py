#!/usr/bin/env python3
"""Local speech and hotkeys for one explicitly selected Codex pane."""

import array
from contextlib import closing
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import socketserver
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
import wave


def has_audio(path):
    """Discard quiet recordings before Whisper can hallucinate speech."""
    with wave.open(str(path), "rb") as wav:
        if (
            wav.getnchannels() != 1
            or wav.getsampwidth() != 2
            or wav.getframerate() != 16000
        ):
            raise RuntimeError("Expected 16 kHz mono PCM recording")
        samples = array.array("h", wav.readframes(wav.getnframes()))
    if len(samples) < 3200:
        return False
    voiced = sum(
        1
        for i in range(0, len(samples), 320)
        if math.sqrt(sum(v * v for v in samples[i : i + 320]) / 320) >= 200
    )
    return voiced >= 8


def spoken_text(text):
    text = re.sub(r"```[^\n]*\n.*?(?:```|\Z)", " ", text, flags=re.S)
    text = re.sub(r"~~~[^\n]*\n.*?(?:~~~|\Z)", " ", text, flags=re.S)
    text = re.sub(r"!?\[([^\]]+)\]\([^\n)]*\)", r"\1", text)
    text = re.sub(r"^\s*(?:#{1,6}\s+|[-*+]\s+|>\s*)", "", text, flags=re.M)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    return " ".join(text.split())


def process_start(pid):
    """Linux process start ticks prevent a reused PID from becoming a target."""
    try:
        return (
            Path(f"/proc/{int(pid)}/stat")
            .read_text()
            .rsplit(")", 1)[1]
            .split()[19]
        )
    except (OSError, ValueError, IndexError):
        return None


def is_cli_thread(thread_id, codex_home=None):
    """Reject inherited subagent callbacks using installed Codex metadata.

    This is deliberately a read-only, fail-closed adapter for schema version 5.
    No transcript contents or unrelated thread records are read.
    """
    home = Path(
        codex_home or os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))
    )
    try:
        uri = (home / "state_5.sqlite").resolve().as_uri() + "?mode=ro"
        with closing(sqlite3.connect(uri, uri=True, timeout=1)) as db:
            row = db.execute(
                "SELECT source FROM threads WHERE id = ?", (thread_id,)
            ).fetchone()
        return row == ("cli",)
    except (OSError, sqlite3.Error):
        return False


class Herdr:
    def input_request(self, target, text):
        # pane.send_text is raw bytes. send_input observes bracketed-paste mode.
        request = {
            "id": uuid.uuid4().hex,
            "method": "pane.send_input",
            "params": {"pane_id": target["pane"], "text": text, "keys": []},
        }
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(8)
            sock.connect(target["socket"])
            sock.sendall(json.dumps(request).encode() + b"\n")
            with sock.makefile("rb") as incoming:
                response = json.loads(incoming.readline(1024 * 1024))
        if "error" in response:
            raise RuntimeError("Could not paste into the selected Codex pane")

    def request(self, target, *args):
        env = dict(os.environ, HERDR_SOCKET_PATH=target["socket"])
        result = subprocess.run(
            ["herdr", *args], env=env, capture_output=True, text=True, timeout=8
        )
        if result.returncode:
            raise RuntimeError("Could not reach the selected Herdr pane")
        return json.loads(result.stdout)["result"]

    def validate(self, target):
        if (
            not target.get("start")
            or process_start(target["pid"]) != target["start"]
        ):
            raise RuntimeError(
                "Selected Codex process has exited; start codex-voice again"
            )
        info = self.request(
            target, "pane", "process-info", "--pane", target["pane"]
        )["process_info"]
        procs = info.get("foreground_processes", [])
        if not any(
            p["pid"] == target["pid"] and "codex" in p.get("name", "")
            for p in procs
        ):
            raise RuntimeError(
                "The selected Codex process is no longer in the foreground"
            )
        agent = self.request(target, "agent", "get", target["pane"])["agent"]
        if agent.get("agent") != "codex":
            raise RuntimeError("The selected pane is not running Codex")
        state = agent.get("agent_status")
        if state not in ("idle", "done"):
            raise RuntimeError(
                f"Codex is {state or 'not ready'}; "
                "finish its current interaction first"
            )

    def insert(self, target, text):
        self.validate(target)
        self.input_request(target, text)

    def submit(self, target):
        self.validate(target)
        # Herdr adds its own blocked-agent and foreground checks here.
        self.request(target, "agent", "prompt", target["pane"], " ")


class Controller:
    def __init__(self, runtime, terminal, audio, notice):
        self.runtime = runtime
        self.terminal, self.audio, self.notice = terminal, audio, notice
        self.lock = threading.RLock()
        self.target = None
        self.token = None
        self.draft = False
        self.cancelled = threading.Event()
        self.reply = None
        self.thread = None
        self.turns = set()
        self.auto = False
        self.playback = None
        self.capture = None
        self.record_cancelled = threading.Event()
        self.transcribing = False
        self.worker = None

    def register(self, token, target, thread=None, turns=()):
        with self.lock:
            self.stop()
            self.token, self.target = token, target
            self.draft = False
            self.reply, self.thread = None, thread
            self.turns = set(turns)
            self._save_selection()

    def _save_selection(self):
        path = self.runtime / "selection.tmp"
        path.write_text(
            json.dumps(
                {
                    "token": self.token,
                    "target": self.target,
                    "thread": self.thread,
                    "turns": sorted(self.turns),
                }
            )
        )
        path.chmod(0o600)
        path.replace(self.runtime / "selection.json")

    def restore(self):
        with self.lock:
            self.target, self.token = None, None
            try:
                saved = json.loads(
                    (self.runtime / "selection.json").read_text()
                )
                target = saved["target"]
                if (
                    not target.get("start")
                    or process_start(target["pid"]) != target["start"]
                ):
                    return False
                self.register(
                    saved["token"],
                    target,
                    saved.get("thread"),
                    saved.get("turns", []),
                )
                return True
            except (OSError, ValueError, KeyError):
                return False

    def unregister(self, token):
        with self.lock:
            if token == self.token:
                self.stop()
                self.target, self.token = None, None
                self.reply, self.thread, self.draft = None, None, False
                (self.runtime / "selection.json").unlink(missing_ok=True)

    def status(self):
        with self.lock:
            return {
                "pane": self.target["pane"] if self.target else None,
                "draft": self.draft,
                "reply": self.reply,
                "auto": self.auto,
                "recording": self.capture is not None,
                "transcribing": self.transcribing,
                "speaking": bool(self.playback and self.playback.is_alive()),
            }

    def notify(self, token, event):
        with self.lock:
            if (
                not self.target
                or token != self.token
                or event.get("type") != "agent-turn-complete"
            ):
                return False
            thread, turn = event.get("thread-id"), event.get("turn-id")
            if (
                not thread
                or not turn
                or (self.thread and self.thread != thread)
            ):
                return False
            if turn in self.turns:
                return False
            self.thread = thread
            self.turns.add(turn)
            self._save_selection()
            self.draft = False
            self.reply = spoken_text(event.get("last-assistant-message") or "")
            if (
                self.auto
                and self.reply
                and not self.capture
                and not self.transcribing
            ):
                self.read(replace=True)
            return True

    def read(self, replace=False):
        with self.lock:
            if self.capture or self.transcribing:
                raise RuntimeError(
                    "Finish recording and transcription before reading a reply"
                )
            playing = bool(self.playback and self.playback.is_alive())
            self.stop()
            if playing and not replace:
                return
            if not self.reply:
                raise RuntimeError("No completed reply to read yet")
            self.cancelled = threading.Event()
            self.playback = threading.Thread(
                target=self._speak,
                args=(self.reply, self.cancelled),
                daemon=True,
            )
            self.playback.start()

    def _speak(self, text, cancelled):
        try:
            self.audio.speak(text, cancelled)
        except Exception as exc:
            if not cancelled.is_set():
                self.notice("Could not read the reply", str(exc))

    def record(self):
        with self.lock:
            if self.capture:
                if self.capture.poll() is None:
                    self.capture.send_signal(signal.SIGINT)
                self.notice("Transcribing", "Processing your recording locally")
                return
            if self.transcribing:
                raise RuntimeError(
                    "Still transcribing; wait for the dictation to appear"
                )
            if not self.target:
                raise RuntimeError("Start codex-voice in a Herdr pane first")
            self.terminal.validate(self.target)
            self.stop()
            self.audio.cue(880)
            self.record_cancelled = threading.Event()
            path = self.runtime / (uuid.uuid4().hex + ".wav")
            self.capture = self.audio.start_capture(path)
            self.worker = threading.Thread(
                target=self._record,
                args=(self.capture, path, self.token, self.record_cancelled),
                daemon=True,
            )
            self.worker.start()
            self.notice(
                "Recording", "Press Super+Space to stop; maximum 3 minutes"
            )

    def _record(self, capture, path, token, cancelled):
        try:
            capture.wait(timeout=185)
            with self.lock:
                if cancelled.is_set():
                    return
                self.capture = None
                self.transcribing = True
            self.audio.cue(660)
            if not has_audio(path):
                raise RuntimeError(
                    "No speech detected; check your microphone and try again"
                )
            text = self.audio.transcribe(path).strip()
            with self.lock:
                if cancelled.is_set():
                    return
                self.stage(text, token)
                self.notice(
                    "Dictation ready", "Super+Shift+Space sends it to Codex"
                )
        except Exception as exc:
            if not cancelled.is_set():
                self.notice("Dictation stopped", str(exc))
        finally:
            if capture.poll() is None:
                capture.terminate()
            path.unlink(missing_ok=True)
            with self.lock:
                if self.record_cancelled is cancelled:
                    self.capture = None
                    self.transcribing = False

    def stage(self, text, token):
        with self.lock:
            if not self.target or token != self.token:
                raise RuntimeError(
                    "Voice session changed; discarded the old transcription"
                )
            if not text.strip():
                raise RuntimeError("No speech detected")
            if any(
                (ord(c) < 32 and c not in "\n\r\t") or 127 <= ord(c) < 160
                for c in text
            ):
                raise RuntimeError(
                    "Discarded dictation containing terminal control characters"
                )
            self.terminal.insert(self.target, text)
            self.draft = True

    def send(self):
        with self.lock:
            if self.capture or self.transcribing:
                raise RuntimeError(
                    "Finish recording and transcription before sending"
                )
            if not self.target or not self.draft:
                raise RuntimeError("No new dictation to send")
            self.terminal.validate(self.target)
            # A failed delivery is ambiguous. Never retry Enter automatically.
            self.draft = False
            self.terminal.submit(self.target)

    def stop(self):
        with self.lock:
            self.cancelled.set()
            self.record_cancelled.set()
            if self.capture and self.capture.poll() is None:
                self.capture.send_signal(signal.SIGINT)
            self.capture = None
            self.transcribing = False
            self.audio.stop()


class LocalAudio:
    def __init__(self, runtime, config):
        self.runtime, self.config = runtime, config
        self.player = None
        self.lock = threading.Lock()
        self.synthesis_lock = threading.Lock()

    def stop(self):
        with self.lock:
            if self.player and self.player.poll() is None:
                self.player.terminate()

    def start_capture(self, path):
        return subprocess.Popen(
            [
                "pw-record",
                "--rate=16000",
                "--channels=1",
                "--format=s16",
                "--sample-count=2880000",
                str(path),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
        )

    def cue(self, frequency):
        with tempfile.NamedTemporaryFile(
            suffix=".wav", dir=self.runtime
        ) as out:
            with wave.open(out.name, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                samples = array.array(
                    "h",
                    (
                        int(
                            750
                            * math.sin(2 * math.pi * frequency * i / 16000)
                            * math.sin(math.pi * i / 1280)
                        )
                        for i in range(1280)
                    ),
                )
                wav.writeframes(samples.tobytes())
            subprocess.run(
                ["pw-play", out.name],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                timeout=3,
                check=False,
            )

    def transcribe(self, path):
        result = subprocess.run(
            [
                "curl",
                "--silent",
                "--show-error",
                "--fail",
                "--max-time",
                "90",
                self.config["stt_url"],
                "-F",
                f"file=@{path}",
                "-F",
                "response_format=json",
                "-F",
                "language=en",
                "-F",
                "temperature=0",
            ],
            capture_output=True,
            text=True,
            timeout=95,
        )
        if result.returncode:
            raise RuntimeError(
                "Local Whisper is unavailable; check codex-voice-stt.service"
            )
        return json.loads(result.stdout).get("text", "")

    def speak(self, text, cancelled):
        # Short chunks bound cancellation latency and VRAM usage.
        chunks, words = [], []
        for word in text.split():
            if words and sum(len(w) + 1 for w in words) + len(word) > 260:
                chunks.append(" ".join(words))
                words = []
            words.append(word)
        if words:
            chunks.append(" ".join(words))
        for chunk in chunks:
            if cancelled.is_set():
                return
            with self.synthesis_lock:
                if cancelled.is_set():
                    return
                request = urllib.request.Request(
                    self.config["tts_url"],
                    data=json.dumps(
                        {
                            "model": "codex-voice",
                            "input": chunk,
                            "language": "English",
                        }
                    ).encode(),
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(request, timeout=90) as response:
                    wav = response.read(32 * 1024 * 1024)
            if cancelled.is_set():
                return
            with tempfile.NamedTemporaryFile(
                suffix=".wav", dir=self.runtime
            ) as out:
                out.write(wav)
                out.flush()
                with self.lock:
                    if cancelled.is_set():
                        return
                    player = subprocess.Popen(
                        ["pw-play", out.name],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                    )
                    self.player = player
                player.wait(timeout=120)
                with self.lock:
                    if self.player is player:
                        self.player = None


def runtime_dir():
    runtime = (
        Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        / "codex-voice"
    )
    runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
    return runtime


def desktop_notice(title, detail=""):
    print(f"{title}: {detail}", file=sys.stderr, flush=True)
    subprocess.run(
        [
            "notify-send",
            "--app-name=Codex Voice",
            "--expire-time=4000",
            "--hint=string:x-canonical-private-synchronous:codex-voice",
            title,
            detail,
        ],
        check=False,
        timeout=5,
    )


def dispatch(app, request):
    action = request["action"]
    if action == "register":
        app.register(request["token"], request["target"])
    elif action == "unregister":
        app.unregister(request["token"])
    elif action == "notify":
        return {"accepted": app.notify(request["token"], request["event"])}
    elif action == "auto":
        with app.lock:
            app.auto = bool(request["enabled"])
    elif action in ("record", "send", "read", "stop"):
        getattr(app, action)()
    elif action != "status":
        raise RuntimeError("Unknown voice command")
    return app.status()


def serve(runtime, config):
    app = Controller(
        runtime, Herdr(), LocalAudio(runtime, config), desktop_notice
    )
    app.auto = config.get("auto_speak", True)
    app.restore()
    path = runtime / "control.sock"
    path.unlink(missing_ok=True)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            try:
                raw = self.rfile.readline(1024 * 1024)
                if not raw.endswith(b"\n"):
                    raise RuntimeError("Voice request exceeds the size limit")
                response = {"ok": True, **dispatch(app, json.loads(raw))}
            except Exception as exc:
                response = {"ok": False, "error": str(exc)}
                desktop_notice("Codex voice", str(exc))
            self.wfile.write(json.dumps(response).encode() + b"\n")

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    with Server(str(path), Handler) as server:
        os.chmod(path, 0o600)

        def shutdown(_sig, _frame):
            app.stop()
            threading.Thread(target=server.shutdown, daemon=True).start()

        signal.signal(signal.SIGTERM, shutdown)
        signal.signal(signal.SIGINT, shutdown)
        monitor_stop = threading.Event()

        def monitor():
            while not monitor_stop.wait(2):
                with app.lock:
                    if (
                        app.target
                        and process_start(app.target["pid"])
                        != app.target["start"]
                    ):
                        app.unregister(app.token)
                        stop_engines()

        threading.Thread(target=monitor, daemon=True).start()
        try:
            server.serve_forever(poll_interval=0.2)
        finally:
            monitor_stop.set()
            app.stop()
            path.unlink(missing_ok=True)


def stop_engines():
    subprocess.run(
        [
            "systemctl",
            "--user",
            "stop",
            "--no-block",
            "codex-voice-stt.service",
            "codex-voice-tts.service",
        ],
        check=False,
        timeout=10,
    )


def call(request, start=True):
    path = runtime_dir() / "control.sock"
    if start:
        subprocess.run(
            ["systemctl", "--user", "start", "codex-voice.service"],
            check=True,
            timeout=15,
        )
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(15)
        # systemd Type=simple may return before the socket is bound.
        for attempt in range(40):
            try:
                sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if not start or attempt == 39:
                    raise RuntimeError("Codex voice service is unavailable")
                time.sleep(0.05)
        sock.sendall(json.dumps(request).encode() + b"\n")
        with sock.makefile("rb") as incoming:
            response = json.loads(incoming.readline(1024 * 1024))
    if not response.get("ok"):
        raise RuntimeError(response.get("error", "Voice command failed"))
    return response


def launch(args):
    if os.environ.get("HERDR_ENV") != "1" or not os.environ.get(
        "HERDR_PANE_ID"
    ):
        raise RuntimeError(
            "Run codex-voice inside the Herdr pane you want to use for voice"
        )
    token = uuid.uuid4().hex
    executable = os.environ.get("CODEX_VOICE_COMMAND", "codex-voice")
    notify = json.dumps([executable, "notify", token])
    subprocess.run(
        [
            "systemctl",
            "--user",
            "start",
            "--no-block",
            "codex-voice-stt.service",
            "codex-voice-tts.service",
        ],
        check=True,
        timeout=15,
    )
    # The user's codex wrapper retains authentication/provider setup.
    child = subprocess.Popen(["codex", "-c", f"notify={notify}", *args])
    previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        target = {
            "pane": os.environ["HERDR_PANE_ID"],
            "socket": os.environ["HERDR_SOCKET_PATH"],
            "pid": child.pid,
            "start": process_start(child.pid),
        }
        call({"action": "register", "token": token, "target": target})
        return child.wait()
    finally:
        signal.signal(signal.SIGINT, previous)
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=10)
        try:
            result = call({"action": "unregister", "token": token}, start=False)
            if result.get("pane") is None:
                stop_engines()
        except (OSError, RuntimeError):
            pass


def main(args=None):
    args = list(sys.argv[1:] if args is None else args)
    try:
        if args and args[0] == "serve":
            config_path = os.environ.get(
                "CODEX_VOICE_CONFIG",
                str(Path.home() / ".config/codex-voice/config.json"),
            )
            serve(runtime_dir(), json.loads(Path(config_path).read_text()))
            return 0
        if args and args[0] == "notify":
            if len(args) != 3:
                return 2
            # Callbacks must not start services or hold up a completed turn.
            event = json.loads(args[2])
            if is_cli_thread(event.get("thread-id")):
                call(
                    {"action": "notify", "token": args[1], "event": event},
                    start=False,
                )
            return 0
        if args and args[0] in (
            "record",
            "send",
            "read",
            "stop",
            "status",
            "auto",
        ):
            request = {"action": args[0]}
            if args[0] == "auto":
                if len(args) != 2 or args[1] not in ("on", "off"):
                    raise RuntimeError("Usage: codex-voice auto on|off")
                request["enabled"] = args[1] == "on"
            response = call(request)
            if args[0] in ("status", "auto"):
                response.pop("reply", None)
                print(json.dumps(response, indent=2))
            return 0
        if args and args[0] in ("--help", "-h"):
            print(
                "Usage: codex-voice [Codex options or resume SESSION]\n"
                "       codex-voice record|send|read|stop|status\n"
                "       codex-voice auto on|off\n\n"
                "Super+Space: record/stop. Super+Shift+Space: send. "
                "Super+R: read/stop.\n"
                "Run inside Herdr. Prefix Codex options with -- "
                "if they conflict.\n"
                "Stop background engines: systemctl --user stop "
                "codex-voice{,-stt,-tts}.service"
            )
            return 0
        return launch(args[1:] if args[:1] == ["--"] else args)
    except Exception as exc:
        print(f"codex-voice: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
