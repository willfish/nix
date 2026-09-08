#!/usr/bin/env python3
"""Local speech and hotkeys for one explicitly selected Codex pane."""

import array
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
import io
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


def dictation_text(text):
    """Whisper's non-speech markers are metadata, never prompt contents."""
    if not isinstance(text, str):
        raise RuntimeError("Whisper returned an invalid transcript")
    text = re.sub(
        r"[\[(]\s*(?:blank[_ ]audio|no[_ ]speech|silence|silent|music|"
        r"inaudible|noise|applause|laughter|breathing)\s*[\])]",
        " ", text, flags=re.I,
    )
    text = text.replace("\u200b", "").replace("\ufeff", "")
    text = re.sub(r"[ \t]+", " ", text).strip()
    return text if any(c.isalnum() for c in text) else ""


def has_control_characters(text):
    return any(
        (ord(c) < 32 and c not in "\n\r\t") or 127 <= ord(c) < 160
        for c in text
    )


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


class DeliveryUncertain(RuntimeError):
    """Input may already be pasted; retrying could duplicate it."""


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
            try:
                sock.connect(target["socket"])
            except OSError as exc:
                raise RuntimeError(
                    "Selected Herdr pane is unavailable"
                ) from exc
            try:
                sock.sendall(json.dumps(request).encode() + b"\n")
                with sock.makefile("rb") as incoming:
                    response = json.loads(incoming.readline(1024 * 1024))
                if response.get("id") != request["id"]:
                    raise ValueError("Mismatched response")
            except (OSError, ValueError) as exc:
                raise DeliveryUncertain(
                    "Could not confirm paste; check the selected Codex prompt "
                    "and send it there if the dictation arrived"
                ) from exc
        if "error" in response:
            raise RuntimeError("Could not paste into the selected Codex pane")

    def request(self, target, *args):
        env = dict(os.environ, HERDR_SOCKET_PATH=target["socket"])
        try:
            result = subprocess.run(
                ["herdr", *args], env=env, capture_output=True,
                text=True, timeout=8,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(
                "Could not reach the selected Herdr pane"
            ) from exc
        if result.returncode:
            raise RuntimeError("Could not reach the selected Herdr pane")
        try:
            return json.loads(result.stdout)["result"]
        except (ValueError, KeyError, TypeError) as exc:
            raise RuntimeError("Invalid response from the Herdr pane") from exc

    def validate_target(self, target):
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
        return agent

    def validate(self, target):
        agent = self.validate_target(target)
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
        self.phase = "idle"
        self.error = None
        self.pending = None
        self.record_started = None
        self.worker = None

    @property
    def transcribing(self):
        return self.phase == "transcribing"

    def recording_active(self):
        return self.phase in (
            "starting", "recording", "stopping", "transcribing"
        )

    def _cue(self, frequency):
        # A disconnected speaker or failed notification must not stop capture.
        def play():
            try:
                self.audio.cue(frequency)
            except Exception:
                print("Codex voice: sound cue unavailable", file=sys.stderr)

        threading.Thread(target=play, daemon=True).start()

    def report_error(self, error):
        with self.lock:
            self.error = str(error)
            if not self.recording_active():
                self.phase = "error"

    def register(self, token, target, thread=None, turns=()):
        with self.lock:
            self.stop()
            self.token, self.target = token, target
            self.draft = False
            self.pending = None
            self.phase = "idle"
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
                self.pending = None
                self.phase = "idle"
                (self.runtime / "selection.json").unlink(missing_ok=True)

    def status(self):
        with self.lock:
            return {
                "pane": self.target["pane"] if self.target else None,
                "draft": self.draft,
                "reply": self.reply,
                "auto": self.auto,
                "recording": self.phase == "recording",
                "transcribing": self.transcribing,
                "speaking": bool(self.playback and self.playback.is_alive()),
                "phase": self.phase,
                "error": self.error,
                "pending": bool(self.pending),
                "recording_seconds": (
                    max(0, time.monotonic() - self.record_started)
                    if self.record_started and self.phase == "recording"
                    else 0
                ),
                "input_level": getattr(self.capture, "level", 0.0),
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
            if self.phase == "draft" and not self.pending:
                self.phase = "idle"
            self.reply = spoken_text(event.get("last-assistant-message") or "")
            if (
                self.auto
                and self.reply
                and not self.recording_active()
                and not self.pending
            ):
                self.read(replace=True)
            return True

    def read(self, replace=False):
        with self.lock:
            if self.pending:
                raise RuntimeError("Send or cancel retained dictation first")
            if self.recording_active():
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
            if self.phase == "starting":
                self.stop()
                self.notice("Recording cancelled", "Microphone was starting")
                return
            if self.phase == "recording":
                self.phase = "stopping"
                threading.Thread(
                    target=self.capture.close, daemon=True
                ).start()
                return
            if self.phase == "stopping":
                return
            if self.transcribing:
                raise RuntimeError(
                    "Still transcribing; wait for the dictation to appear"
                )
            if not self.target:
                raise RuntimeError("Start codex-voice in a Herdr pane first")
            self.stop()
            self.draft = False
            self.pending = None
            self.error = None
            self.phase = "starting"
            self.record_cancelled = threading.Event()
            path = self.runtime / (uuid.uuid4().hex + ".wav")
            self.worker = threading.Thread(
                target=self._record,
                args=(path, self.token, self.target, self.record_cancelled),
                daemon=True,
            )
            self.worker.start()

    def _record(self, path, token, target, cancelled):
        capture = None
        try:
            # Readiness can be transiently unknown while Codex redraws. Check
            # identity now and enforce idle/done only when delivering text.
            self.terminal.validate_target(target)
            if cancelled.is_set():
                return
            capture = self.audio.start_capture(path)
            with self.lock:
                if cancelled.is_set():
                    return
                self.capture = capture
            if not capture.wait_ready(timeout=5):
                raise RuntimeError("Microphone did not become ready")
            with self.lock:
                if cancelled.is_set():
                    return
                self.record_started = time.monotonic()
                self.phase = "recording"
            self._cue(880)
            self.notice(
                "Recording", "Press Super+Space to stop; maximum 3 minutes"
            )
            code = capture.wait(timeout=185)
            with self.lock:
                if cancelled.is_set():
                    return
                if code not in (0, -signal.SIGINT) or getattr(
                    capture, "error", None
                ):
                    raise RuntimeError(
                        getattr(capture, "error", None)
                        or "Microphone capture failed; check the input device"
                    )
                self.capture = None
                self.phase = "transcribing"
            self._cue(660)
            if not has_audio(path):
                text = ""
            else:
                text = dictation_text(self.audio.transcribe(path))
            with self.lock:
                if cancelled.is_set():
                    return
                try:
                    staged = self.stage(text, token)
                except DeliveryUncertain:
                    # A lost acknowledgement is different from a failed
                    # connection. Never paste or press Enter again blindly.
                    self.pending = None
                    self.draft = False
                    raise
                except RuntimeError as exc:
                    # Keep a valid transcript when the pane is temporarily
                    # busy/unreachable. Never paste it into another session.
                    if text and not has_control_characters(text):
                        self.pending = text
                        self.phase = "draft"
                        self.notice(
                            "Dictation retained",
                            f"{exc}. Press Send when Codex is ready.",
                        )
                        return
                    raise
                if staged:
                    self.notice(
                        "Dictation ready", "Super+Shift+Space sends it to Codex"
                    )
                else:
                    self.notice("No speech detected", "Nothing was inserted")
        except Exception as exc:
            with self.lock:
                if (
                    not cancelled.is_set()
                    and self.record_cancelled is cancelled
                ):
                    self.error = str(exc)
                    self.phase = "error"
                    self.notice("Dictation stopped", str(exc))
        finally:
            try:
                if capture:
                    capture.close()
            finally:
                path.unlink(missing_ok=True)
                with self.lock:
                    if self.record_cancelled is cancelled:
                        self.capture = None
                        self.record_started = None
                        if self.recording_active():
                            self.phase = "idle"

    def stage(self, text, token):
        with self.lock:
            if not self.target or token != self.token:
                raise RuntimeError(
                    "Voice session changed; discarded the old transcription"
                )
            text = dictation_text(text)
            if not text:
                self.draft = False
                self.pending = None
                self.phase = "idle"
                return False
            if has_control_characters(text):
                raise RuntimeError(
                    "Discarded dictation containing terminal control characters"
                )
            try:
                self.terminal.insert(self.target, text)
            except DeliveryUncertain:
                # This also covers Send retrying a previously retained draft.
                self.pending = None
                self.draft = False
                raise
            self.draft = True
            self.pending = None
            self.phase = "draft"
            self.error = None
            return True

    def send(self):
        with self.lock:
            if self.recording_active():
                raise RuntimeError(
                    "Finish recording and transcription before sending"
                )
            if self.target and self.pending:
                self.stage(self.pending, self.token)
            if not self.target or not self.draft:
                raise RuntimeError("No new dictation to send")
            self.terminal.validate(self.target)
            # A failed delivery is ambiguous. Never retry Enter automatically.
            self.draft = False
            self.phase = "idle"
            self.terminal.submit(self.target)

    def stop(self):
        with self.lock:
            self.cancelled.set()
            self.record_cancelled.set()
            if self.capture:
                threading.Thread(
                    target=self.capture.close, daemon=True
                ).start()
            self.capture = None
            self.record_started = None
            self.pending = None
            self.phase = "draft" if self.draft else "idle"
            self.error = None
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
        from voice_capture import PipeWireCapture

        return PipeWireCapture(path)

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

    def _synthesize(self, chunk, cancelled):
        with self.synthesis_lock:
            if cancelled.is_set():
                return None
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
                data = response.read(32 * 1024 * 1024)
        if cancelled.is_set():
            return None
        with wave.open(io.BytesIO(data), "rb") as wav:
            params = wav.getparams()
            frames = wav.readframes(wav.getnframes())
            if not frames or len(frames) != (
                wav.getnframes() * wav.getnchannels() * wav.getsampwidth()
            ):
                raise RuntimeError("Incomplete speech audio")
        return params, frames

    def _stream(self, chunks, cancelled):
        prepared = self._synthesize(chunks[0], cancelled)
        if prepared is None:
            return
        params, frames = prepared
        formats = {1: "u8", 2: "s16", 3: "s24", 4: "s32"}
        with ThreadPoolExecutor(max_workers=1) as worker:
            with self.lock:
                if cancelled.is_set():
                    return
                player = subprocess.Popen(
                    [
                        "pw-play",
                        "--raw",
                        "--format", formats[params.sampwidth],
                        "--rate", str(params.framerate),
                        "--channels", str(params.nchannels),
                        "-",
                    ],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                )
                self.player = player
            try:
                for index in range(len(chunks)):
                    if cancelled.is_set():
                        return
                    # One chunk ahead, with pipe backpressure bounding memory.
                    pending = (
                        worker.submit(
                            self._synthesize, chunks[index + 1], cancelled
                        )
                        if index + 1 < len(chunks)
                        else None
                    )
                    player.stdin.write(frames)
                    player.stdin.flush()
                    if pending is not None:
                        prepared = pending.result()
                        if prepared is None:
                            return
                        next_params, frames = prepared
                        if next_params[:3] != params[:3]:
                            raise RuntimeError("Speech audio format changed")
                player.stdin.close()
                if player.wait() and not cancelled.is_set():
                    raise RuntimeError("Speech playback failed")
            except BrokenPipeError:
                if not cancelled.is_set():
                    raise RuntimeError(
                        "Speech playback stopped unexpectedly"
                    ) from None
            finally:
                with self.lock:
                    if self.player is player:
                        self.player = None
                if player.poll() is None:
                    player.terminate()
                    try:
                        player.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        player.kill()
                        player.wait()
                if not player.stdin.closed:
                    try:
                        player.stdin.close()
                    except BrokenPipeError:
                        pass

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
        if not chunks or cancelled.is_set():
            return
        if self.config.get("playback_mode", "buffered") == "streaming":
            self._stream(chunks, cancelled)
            return
        # An unnamed buffer also disappears if the controller exits abruptly.
        with tempfile.TemporaryFile(suffix=".wav", dir=self.runtime) as out:
            # Finish the entire WAV before playing, without parallel GPU work.
            with ExitStack() as stack:
                combined = None
                for chunk in chunks:
                    if cancelled.is_set():
                        return
                    prepared = self._synthesize(chunk, cancelled)
                    if prepared is None:
                        return
                    params, frames = prepared
                    if combined is None:
                        combined = stack.enter_context(wave.open(out, "wb"))
                        combined.setparams(params)
                    elif combined.getparams()[:3] != params[:3]:
                        raise RuntimeError("Speech audio format changed")
                    combined.writeframesraw(frames)
            out.flush()
            out.seek(0)
            with self.lock:
                if cancelled.is_set():
                    return
                player = subprocess.Popen(
                    ["pw-play", f"/proc/self/fd/{out.fileno()}"],
                    pass_fds=(out.fileno(),),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                )
                self.player = player
            try:
                # Full replies can exceed two minutes; stop() interrupts this.
                player.wait()
            finally:
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
    try:
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
    except (OSError, subprocess.TimeoutExpired):
        pass


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
    from codex_voice_tray import VoiceTray

    app = Controller(
        runtime, Herdr(), LocalAudio(runtime, config), desktop_notice
    )
    app.auto = config.get("auto_speak", True)
    app.restore()
    def tray_action(action):
        try:
            dispatch(app, {"action": action})
        except Exception as exc:
            app.report_error(exc)
            desktop_notice("Codex voice", str(exc))

    tray = VoiceTray(app.status, tray_action)
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
                app.report_error(exc)
                desktop_notice("Codex voice", str(exc))
            self.wfile.write(json.dumps(response).encode() + b"\n")

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    with Server(str(path), Handler) as server:
        os.chmod(path, 0o600)
        tray.start()

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
            tray.stop()
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
