"""Local microphone, recognition and speech playback adapters."""

import array
import io
import json
import math
from pathlib import Path
import re
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
import subprocess
import tempfile
import threading
import time
import os
import urllib.error
import urllib.parse
import urllib.request
import uuid
import wave

from voice_devices import MicrophoneMonitor


def speech_chunks(text, maximum=260, first=120):
    """Prefer complete sentences, then clauses, then word boundaries."""
    remaining = " ".join(text.split())
    chunks = []
    while remaining:
        limit = min(first if not chunks else maximum, maximum)
        if len(remaining) <= limit:
            chunks.append(remaining)
            break
        end = None
        for punctuation in (r"[.!?][\"')\]]*", r"[,;:][\"')\]]*"):
            boundaries = [
                match.end() for match in re.finditer(
                    punctuation + r"(?=\s|$)", remaining[:maximum + 1]
                ) if match.end() <= maximum
            ]
            shorter = [position for position in boundaries if position <= limit]
            if shorter or boundaries:
                end = shorter[-1] if shorter else boundaries[0]
                break
        if end is None:
            end = remaining.rfind(" ", 0, limit + 1)
            if end <= 0:
                end = limit
        chunks.append(remaining[:end].rstrip())
        remaining = remaining[end:].lstrip()
    return chunks


class LocalAudio:
    def __init__(self, runtime, config, *, engines=None):
        self.engines = engines
        self.runtime, self.config = runtime, config
        self.player = None
        self.lock = threading.Lock()
        self.voices = {
            "samantha": {"label": "Samantha", "options": {}},
            **config.get("tts_voices", {}),
        }
        self.selected_voice = "samantha"
        preferences = config.get("voice_preferences_path")
        self.voice_preferences = Path(preferences) if preferences else None
        if self.voice_preferences and self.voice_preferences.exists():
            saved = self.voice_preferences.read_text().strip()
            # Previous Samantha modes and removed characters return to Samantha.
            if saved in self.voices:
                self.selected_voice = saved
        self.stt_backend = "whisper"
        stt_path = config.get("stt_preferences_path")
        if not stt_path and self.voice_preferences:
            stt_path = self.voice_preferences.with_name("stt-backend")
        self.stt_preferences = Path(stt_path) if stt_path else None
        if self.stt_preferences and self.stt_preferences.exists():
            saved_stt = self.stt_preferences.read_text().strip()
            if saved_stt in ("whisper", "deepgram"):
                self.stt_backend = saved_stt
        self.synthesis_lock = threading.Lock()
        self.recognition_lock = threading.Lock()
        self.stopped = threading.Event()
        self.backend_lock = threading.Lock()
        self.backends = {"stt": "unknown", "tts": "unknown"}
        self.backend_errors = {}
        self.backend_updated = {"stt": 0, "tts": 0}
        self.backend_refreshing = set()
        self.microphone = MicrophoneMonitor(config.get("preferred_microphone"))

    def _backend_state(self, engine, state, error=None):
        with self.backend_lock:
            self.backends[engine] = state
            self.backend_updated[engine] = time.monotonic()
            if error:
                self.backend_errors[engine] = str(error)
            else:
                self.backend_errors.pop(engine, None)

    def _probe(self, engine, timeout):
        url = self.config.get(f"{engine}_health_url")
        if not url:
            return True
        try:
            with urllib.request.urlopen(url, timeout=timeout) as response:
                payload = json.loads(response.read(16384))
        except urllib.error.HTTPError as exc:
            exc.close()
            if exc.code == 503:
                return False
            raise RuntimeError(
                f"{self._engine_name(engine)} health endpoint returned "
                f"HTTP {exc.code}; check pi-voice-{engine}.service"
            ) from None
        except (OSError, urllib.error.URLError):
            return False
        except (ValueError, UnicodeDecodeError):
            raise RuntimeError(
                f"{self._engine_name(engine)} returned invalid health data"
            ) from None
        if not isinstance(payload, dict):
            raise RuntimeError(
                f"{self._engine_name(engine)} returned invalid health data"
            )
        if engine == "stt":
            return payload.get("status") == "ok"
        model_id = self.config.get("tts_model", "pi-voice")
        models = payload.get("data")
        if not isinstance(models, list):
            raise RuntimeError("Samantha TTS returned invalid model data")
        for model in models:
            if isinstance(model, dict) and model.get("id") == model_id:
                return model.get("loaded") is True
        raise RuntimeError(
            f"Samantha TTS model '{model_id}' is missing from the server; "
            "check pi-voice-tts.service and model files"
        )

    @staticmethod
    def _engine_name(engine):
        return "Whisper" if engine == "stt" else "Samantha TTS"

    def readiness(self, engine, timeout):
        return self.wait_ready(engine, timeout=timeout)

    def wait_ready(self, engine, cancelled=None, timeout=None):
        cancelled = cancelled or threading.Event()
        if not self.config.get(f"{engine}_health_url"):
            return not cancelled.is_set()
        deadline = time.monotonic() + float(
            self.config.get("readiness_timeout",
                            60) if timeout is None else timeout
        )
        self._backend_state(engine, "loading")
        while not cancelled.is_set():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                error = RuntimeError(
                    f"{self._engine_name(engine)} model is not ready; "
                    f"check pi-voice-{engine}.service and model files"
                )
                self._backend_state(engine, "error", error)
                raise error
            try:
                ready = self._probe(engine, min(.25, remaining))
            except RuntimeError as exc:
                self._backend_state(engine, "error", exc)
                raise
            if ready:
                self._backend_state(engine, "ready")
                return not cancelled.is_set()
            cancelled.wait(min(.1, max(0, deadline - time.monotonic())))
        return False

    def set_voice(self, character):
        if character not in self.voices:
            raise RuntimeError(
                "Unknown voice; choose " + ", ".join(self.voices)
            )
        with self.lock:
            if self.voice_preferences:
                temporary = self.voice_preferences.with_suffix(".tmp")
                temporary.write_text(character + "\n")
                temporary.replace(self.voice_preferences)
            self.selected_voice = character

    def set_stt_backend(self, backend):
        if backend not in ("whisper", "deepgram"):
            raise RuntimeError("Unknown dictation backend")
        if backend == "deepgram" and not os.environ.get("DEEPGRAM_API_KEY"):
            raise RuntimeError(
                "Deepgram API key is not installed; stay on Whisper"
            )
        with self.lock:
            if self.stt_preferences:
                temporary = self.stt_preferences.with_suffix(".tmp")
                temporary.write_text(backend + "\n")
                temporary.replace(self.stt_preferences)
            self.stt_backend = backend

    def status(self):
        """Return cached health immediately, refreshing in bounded workers."""
        def refresh(engine):
            try:
                ready = self._probe(engine, .25)
                self._backend_state(engine, "ready" if ready else "loading")
            except RuntimeError as exc:
                self._backend_state(engine, "error", exc)
            finally:
                with self.backend_lock:
                    self.backend_refreshing.discard(engine)

        with self.backend_lock:
            result = {
                "selected_voice": self.selected_voice,
                "voices": {
                    key: voice["label"] for key, voice in self.voices.items()
                },
                "selected_stt": self.stt_backend,
                "stt_backends": {
                    "whisper": "Whisper (local GPU)",
                    "deepgram": "Deepgram (cloud)",
                },
                "backends": dict(self.backends),
                "backend_errors": dict(self.backend_errors),
            }
            for engine in self.backends:
                if (
                    self.config.get(f"{engine}_health_url")
                    and engine not in self.backend_refreshing
                    and time.monotonic() - self.backend_updated[engine] > 2
                ):
                    self.backend_refreshing.add(engine)
                    threading.Thread(
                        target=refresh, args=(engine,), daemon=True
                    ).start()
        result["microphone"] = self.microphone.status()
        return result

    def stop(self):
        with self.lock:
            self.stopped.set()
            self.stopped = threading.Event()
            if self.player and self.player.poll() is None:
                self.player.terminate()

    def _request(self, operation, gate, cancelled, engine=None, manager=None,
                 on_drained=None):
        """Cancel the caller while draining already submitted GPU work."""
        with self.lock:
            stopped = self.stopped
        finished = threading.Event()
        outcome = []

        def abandoned():
            return cancelled.is_set() or stopped.is_set()

        def perform():
            lease = None
            try:
                owner = manager or self.engines
                if owner and engine:
                    lease = owner.acquire(engine)
                    while not abandoned():
                        try:
                            lease.wait(timeout=.05)
                            break
                        except TimeoutError:
                            if lease.ready.done():
                                raise
                    if abandoned():
                        return
                while not abandoned():
                    if gate.acquire(timeout=.05):
                        break
                else:
                    return
                try:
                    if not abandoned():
                        outcome.append((True, operation()))
                finally:
                    gate.release()
            except Exception as exc:
                outcome.append((False, exc))
            finally:
                if lease:
                    lease.release()
                # No audio/gate lock is held across controller callbacks.
                # Success is reported even if cancellation raced HTTP
                # completion. Callers
                # must bound callback shutdown; failures cannot strand waiters.
                try:
                    if on_drained and outcome and outcome[0][0]:
                        on_drained(outcome[0][1])
                finally:
                    finished.set()

        threading.Thread(target=perform, daemon=True).start()
        while not finished.wait(.025):
            if abandoned():
                return None
        if abandoned() or not outcome:
            return None
        succeeded, value = outcome[0]
        if not succeeded:
            raise value
        return value

    def _http(self, request, engine, maximum):
        name = self._engine_name(engine)
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                data = response.read(maximum + 1)
            if len(data) > maximum:
                raise RuntimeError(f"{name} response exceeds the size limit")
        except urllib.error.HTTPError as exc:
            exc.close()
            error = RuntimeError(
                f"{name} request failed (HTTP {exc.code}); "
                f"check pi-voice-{engine}.service"
            )
            self._backend_state(engine, "error", error)
            raise error from None
        except urllib.error.URLError:
            error = RuntimeError(
                f"{name} connection failed; "
                f"check pi-voice-{engine}.service"
            )
            self._backend_state(engine, "error", error)
            raise error from None
        self._backend_state(engine, "ready")
        return data

    def start_capture(self, path):
        from voice_capture import PipeWireCapture

        selected = self.microphone.resolve()
        return PipeWireCapture(path, target=selected["target"])

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

    def transcribe_drained(self, path, cancelled, on_drained):
        """Report each successful drained request once, including cancelled
        work.

        on_drained runs on the request worker without audio locks and must bound
        shutdown waits. It must never stage text. Legacy adapters can omit
        this method.
        """
        return self.transcribe(path, cancelled, on_drained=on_drained)

    def _deepgram_url(self):
        params = [
            ("model", "nova-3"),
            ("smart_format", "true"),
            ("punctuate", "true"),
            ("mip_opt_out", "true"),
        ]
        for term in (self.config.get("stt_prompt") or "").split(","):
            term = term.strip()
            if term:
                params.append(("keyterm", term))
        return "https://api.deepgram.com/v1/listen?" + urllib.parse.urlencode(
            params
        )

    @staticmethod
    def _deepgram_text(result):
        if isinstance(result.get("text"), str) and result["text"].strip():
            return result["text"]
        channels = (result.get("results") or {}).get("channels") or []
        if not channels or not isinstance(channels[0], dict):
            raise RuntimeError("Deepgram returned no transcription text")
        alternatives = channels[0].get("alternatives") or []
        if not alternatives or not isinstance(alternatives[0], dict):
            raise RuntimeError("Deepgram returned no transcription text")
        text = alternatives[0].get("transcript")
        if not isinstance(text, str):
            raise RuntimeError("Deepgram returned no transcription text")
        return text

    def _transcribe_deepgram(self, path, cancelled, on_drained):
        key = os.environ.get("DEEPGRAM_API_KEY")
        if not key:
            raise RuntimeError(
                "Deepgram API key is not installed; stay on Whisper"
            )
        payload = path.read_bytes()

        def recognize():
            request = urllib.request.Request(
                self._deepgram_url(), data=payload,
                headers={
                    "Authorization": "Token " + key,
                    "Content-Type": "audio/wav",
                },
            )
            try:
                with urllib.request.urlopen(request, timeout=90) as response:
                    data = response.read(1024 * 1024 + 1)
            except urllib.error.HTTPError as exc:
                exc.close()
                raise RuntimeError(
                    f"Deepgram request failed (HTTP {exc.code})"
                ) from None
            except urllib.error.URLError as exc:
                raise RuntimeError("Deepgram connection failed") from exc
            if len(data) > 1024 * 1024:
                raise RuntimeError("Deepgram response exceeds the size limit")
            try:
                result = json.loads(data)
            except (ValueError, UnicodeDecodeError):
                raise RuntimeError("Deepgram returned invalid JSON") from None
            if not isinstance(result, dict):
                raise RuntimeError("Deepgram returned invalid JSON")
            return self._deepgram_text(result)

        return self._request(
            recognize, self.recognition_lock, cancelled, on_drained=on_drained
        ) or ""

    def transcribe(self, path, cancelled=None, *, on_drained=None):
        cancelled = cancelled or threading.Event()
        if self.stt_backend == "deepgram":
            return self._transcribe_deepgram(path, cancelled, on_drained)
        if not self.wait_ready("stt", cancelled):
            return ""
        boundary = uuid.uuid4().hex
        fields = {
            "response_format": "json",
            "language": self.config.get("stt_language", "en"),
            "temperature": "0",
        }
        if self.config.get("stt_prompt"):
            fields["prompt"] = self.config["stt_prompt"]
        body = bytearray()
        for name, value in fields.items():
            body.extend((
                f'--{boundary}\r\nContent-Disposition: form-data; '
                f'name="{name}"\r\n\r\n{value}\r\n'
            ).encode())
        body.extend((
            f'--{boundary}\r\nContent-Disposition: form-data; '
            'name="file"; filename="dictation.wav"\r\n'
            'Content-Type: audio/wav\r\n\r\n'
        ).encode())
        body.extend(path.read_bytes())
        body.extend(f"\r\n--{boundary}--\r\n".encode())

        def recognize():
            request = urllib.request.Request(
                self.config["stt_url"], data=bytes(body),
                headers={
                    "Content-Type": f"multipart/form-data; boundary={boundary}",
                },
            )
            data = self._http(request, "stt", 1024 * 1024)
            try:
                result = json.loads(data)
            except (ValueError, UnicodeDecodeError):
                raise RuntimeError("Whisper returned invalid JSON") from None
            if not isinstance(result, dict) or not isinstance(
                result.get("text"), str
            ):
                raise RuntimeError("Whisper returned no transcription text")
            return result["text"]

        return self._request(
            recognize, self.recognition_lock, cancelled, engine='stt',
            on_drained=on_drained
        ) or ""

    def _synthesize(self, chunk, cancelled, voice_options):
        def synthesize():
            request = urllib.request.Request(
                self.config["tts_url"],
                data=json.dumps(
                    {
                        "model": self.config.get("tts_model", "pi-voice"),
                        "input": chunk,
                        "language": "English",
                        **voice_options,
                    }
                ).encode(),
                headers={"Content-Type": "application/json"},
            )
            return self._http(request, "tts", 32 * 1024 * 1024)

        data = self._request(synthesize, self.synthesis_lock,
                             cancelled, engine='tts')
        if data is None:
            return None
        with wave.open(io.BytesIO(data), "rb") as wav:
            params = wav.getparams()
            frames = wav.readframes(wav.getnframes())
            if not frames or len(frames) != (
                wav.getnframes() * wav.getnchannels() * wav.getsampwidth()
            ):
                raise RuntimeError("Incomplete speech audio")
        return params, frames

    def _stream(self, chunks, cancelled, voice_options):
        prepared = self._synthesize(chunks[0], cancelled, voice_options)
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
                            self._synthesize, chunks[index + 1], cancelled,
                            voice_options
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
        # Select once for the complete spoken reply, before chunking or waits.
        character = self.selected_voice
        voice_options = dict(self.voices[character]["options"])
        if character == "samantha" and len(text.split()) > 50:
            voice_options = dict(self.config.get("tts_long_voice", {}))
        chunks = speech_chunks(text)
        if not chunks or cancelled.is_set():
            return
        if not self.wait_ready("tts", cancelled):
            return
        if self.config.get("playback_mode", "buffered") == "streaming":
            self._stream(chunks, cancelled, voice_options)
            return
        # An unnamed buffer also disappears if the controller exits abruptly.
        with tempfile.TemporaryFile(suffix=".wav", dir=self.runtime) as out:
            # Finish the entire WAV before playing, without parallel GPU work.
            with ExitStack() as stack:
                combined = None
                for chunk in chunks:
                    if cancelled.is_set():
                        return
                    prepared = self._synthesize(chunk, cancelled, voice_options)
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
