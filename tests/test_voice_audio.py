"""Audio boundaries, request lifetime and readiness behavior."""

import importlib.util
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import wave

MODULE = (
    Path(__file__).resolve().parents[1] / "home/config/voice/voice_audio.py"
)
sys.path.insert(0, str(MODULE.parent))
spec = importlib.util.spec_from_file_location("voice_audio", MODULE)
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)


def wav_bytes():
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(24000)
        wav.writeframes(b"\0\0" * 24)
    return output.getvalue()


@contextmanager
def server(callback):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.handle_request(b"")

        def do_POST(self):
            self.handle_request(self.rfile.read(int(
                self.headers.get("Content-Length", 0)
            )))

        def handle_request(self, body):
            code, response = callback(self.path, body)
            self.send_response(code)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            try:
                self.wfile.write(response)
            except BrokenPipeError:
                pass

        def log_message(self, *args):
            pass

    instance = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=instance.serve_forever, daemon=True)
    worker.start()
    try:
        yield f"http://127.0.0.1:{instance.server_port}"
    finally:
        instance.shutdown()
        instance.server_close()
        worker.join(2)


class ChunkTests(unittest.TestCase):
    def test_chunks_end_at_sentences_and_start_with_a_short_phrase(self):
        first = "The voice is now ready to use."
        second = "The recorder waits for your microphone before it starts."
        third = "Dictation stays in your selected terminal until you send it."
        fourth = "You can cancel recording or playback from the tray."
        chunks = audio.speech_chunks(" ".join([first, second, third, fourth]))
        self.assertEqual(chunks, [first + " " + second, third + " " + fourth])

    def test_normal_first_sentence_is_not_cut_just_to_hit_short_target(self):
        sentence = "The recorder " + "captures your voice " * 8 + "reliably."
        self.assertGreater(len(sentence), 120)
        self.assertEqual(audio.speech_chunks(sentence), [sentence])

    def test_long_sentence_uses_clause_boundaries(self):
        clause = "These changes keep " + "all of your words " * 4 + "safe;"
        text = clause + " " + "recognition " * 30
        self.assertEqual(audio.speech_chunks(text)[0], clause)

    def test_oversized_words_are_bounded_and_never_dropped(self):
        word = "x" * 811
        chunks = audio.speech_chunks(word)
        self.assertEqual("".join(chunks), word)
        self.assertTrue(all(0 < len(chunk) <= 260 for chunk in chunks))
        self.assertEqual(len(chunks[0]), 120)


class RequestTests(unittest.TestCase):
    def setUp(self):
        self.microphone_patch = patch.object(audio, "MicrophoneMonitor")
        self.microphone = self.microphone_patch.start().return_value
        self.microphone.status.return_value = {"name": "Test microphone"}
        self.addCleanup(self.microphone_patch.stop)

    def test_capture_resolves_preferred_device_and_reports_current_name(self):
        with tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {
                "preferred_microphone": "usb.microphone",
            })
            self.microphone.resolve.return_value = {
                "target": "usb.microphone", "name": "Test microphone",
            }
            path = Path(directory) / "capture.wav"
            with patch("voice_capture.PipeWireCapture") as capture:
                self.assertIs(local.start_capture(path), capture.return_value)
                capture.assert_called_once_with(path, target="usb.microphone")
            self.assertEqual(
                local.status()["microphone"]["name"], "Test microphone"
            )

    def test_recognition_failure_names_the_server_and_preserves_recording(self):
        with (
            server(lambda path, body: (500, b"failed")) as url,
            tempfile.TemporaryDirectory() as directory,
        ):
            path = Path(directory) / "dictation.wav"
            path.write_bytes(wav_bytes())
            local = audio.LocalAudio(Path(directory), {"stt_url": url})
            with self.assertRaisesRegex(RuntimeError, "Whisper.*HTTP 500"):
                local.transcribe(path)
            self.assertTrue(path.is_file())

    def test_recognition_rejects_invalid_json_with_a_specific_error(self):
        with (
            server(lambda path, body: (200, b"not-json")) as url,
            tempfile.TemporaryDirectory() as directory,
        ):
            path = Path(directory) / "dictation.wav"
            path.write_bytes(wav_bytes())
            local = audio.LocalAudio(Path(directory), {"stt_url": url})
            with self.assertRaisesRegex(RuntimeError, "Whisper.*invalid JSON"):
                local.transcribe(path)

    def test_synthesis_failure_names_the_model_server_without_playback(self):
        with (
            server(lambda path, body: (503, b"busy")) as url,
            tempfile.TemporaryDirectory() as directory,
            patch.object(audio.subprocess, "Popen") as player,
        ):
            local = audio.LocalAudio(Path(directory), {"tts_url": url})
            with self.assertRaisesRegex(RuntimeError, "Samantha.*HTTP 503"):
                local.speak("Hello.", threading.Event())
            player.assert_not_called()

    def test_tts_readiness_waits_until_the_configured_model_is_loaded(self):
        checks = []

        def respond(path, body):
            checks.append(path)
            model = {"id": "codex-voice", "loaded": len(checks) > 1}
            return 200, json.dumps({"data": [model]}).encode()

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {
                "tts_health_url": url, "readiness_timeout": 1,
            })
            self.assertTrue(local.wait_ready("tts"))
            self.assertGreaterEqual(len(checks), 2)

    def test_missing_tts_model_reports_the_configuration_error(self):
        with (
            server(lambda path, body: (200, b'{"data":[]}')) as url,
            tempfile.TemporaryDirectory() as directory,
        ):
            local = audio.LocalAudio(Path(directory), {
                "tts_health_url": url, "readiness_timeout": 1,
            })
            with self.assertRaisesRegex(RuntimeError, "codex-voice.*missing"):
                local.wait_ready("tts")

    def test_status_refreshes_health_without_waiting_for_slow_response(self):
        entered = threading.Event()
        release = threading.Event()

        def respond(path, body):
            entered.set()
            release.wait(1)
            return 200, b'{"status":"ok"}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {"stt_health_url": url})
            try:
                start = time.monotonic()
                local.status()
                self.assertLess(time.monotonic() - start, .1)
                self.assertTrue(entered.wait(1))
                release.set()
                deadline = time.monotonic() + 1
                while local.status()["backends"]["stt"] != "ready":
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
            finally:
                release.set()

    def test_recognition_waits_for_model_startup_and_reports_ready(self):
        checks = []

        def respond(path, body):
            if path == "/health":
                checks.append(path)
                if len(checks) == 1:
                    return 503, b'{"status":"loading"}'
                return 200, b'{"status":"ok"}'
            return 200, b'{"text":"ready words"}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dictation.wav"
            path.write_bytes(wav_bytes())
            local = audio.LocalAudio(Path(directory), {
                "stt_url": url + "/inference",
                "stt_health_url": url + "/health",
                "readiness_timeout": 1,
            })
            self.assertEqual(local.transcribe(path), "ready words")
            self.assertGreaterEqual(len(checks), 2)
            self.assertEqual(local.status()["backends"]["stt"], "ready")

    def test_unavailable_model_has_bounded_specific_error(self):
        def respond(path, body):
            return 503, b'{"status":"loading"}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {
                "stt_health_url": url, "readiness_timeout": .15,
            })
            start = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "Whisper.*not ready"):
                local.wait_ready("stt", threading.Event())
            self.assertLess(time.monotonic() - start, .6)
            self.assertEqual(local.status()["backends"]["stt"], "error")

    def test_cancel_interrupts_model_readiness(self):
        entered = threading.Event()
        cancelled = threading.Event()
        done = threading.Event()
        results = []

        def respond(path, body):
            entered.set()
            return 503, b'{"status":"loading"}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {
                "tts_health_url": url, "readiness_timeout": 2,
            })

            def wait():
                results.append(local.wait_ready("tts", cancelled))
                done.set()

            worker = threading.Thread(target=wait)
            worker.start()
            try:
                self.assertTrue(entered.wait(1))
                cancelled.set()
                self.assertTrue(done.wait(.5))
                self.assertEqual(results, [False])
            finally:
                cancelled.set()
                worker.join(3)

    def test_recognition_sends_vocabulary_and_leaves_source_for_retry(self):
        requests = []

        def respond(path, body):
            requests.append(body)
            return 200, b'{"text":"Check Herdr and NixOS."}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dictation.wav"
            path.write_bytes(wav_bytes())
            local = audio.LocalAudio(Path(directory), {
                "stt_url": url, "stt_prompt": "Herdr, NixOS, Andromeda",
            })
            text = local.transcribe(path, threading.Event())
            self.assertEqual(text, "Check Herdr and NixOS.")
            self.assertEqual(path.read_bytes(), wav_bytes())
        self.assertIn(b'name="prompt"', requests[0])
        self.assertIn(b"Herdr, NixOS, Andromeda", requests[0])
        self.assertIn(b'name="file"', requests[0])

    def test_cancelled_recognition_returns_without_deleting_retry_audio(self):
        entered = threading.Event()
        release = threading.Event()
        cancelled = threading.Event()
        done = threading.Event()
        results = []

        def respond(path, body):
            entered.set()
            release.wait(3)
            return 200, b'{"text":"late words"}'

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dictation.wav"
            path.write_bytes(wav_bytes())
            local = audio.LocalAudio(Path(directory), {"stt_url": url})

            def transcribe():
                try:
                    results.append(local.transcribe(path, cancelled))
                finally:
                    done.set()

            worker = threading.Thread(target=transcribe)
            worker.start()
            try:
                self.assertTrue(entered.wait(2))
                cancelled.set()
                self.assertTrue(done.wait(.5))
                self.assertEqual(results, [""])
                self.assertTrue(path.is_file())
            finally:
                release.set()
                worker.join(3)

    def test_cancel_returns_before_http_but_keeps_gpu_work_serial(self):
        entered = threading.Event()
        release = threading.Event()
        second_request = threading.Event()
        first_done = threading.Event()
        cancelled = threading.Event()
        requests = []
        errors = []

        def respond(path, body):
            requests.append(json.loads(body)["input"])
            if len(requests) == 1:
                entered.set()
                release.wait(3)
            else:
                second_request.set()
            return 200, wav_bytes()

        with server(respond) as url, tempfile.TemporaryDirectory() as directory:
            local = audio.LocalAudio(Path(directory), {"tts_url": url})

            def speak(text, event, done=None):
                try:
                    local.speak(text, event)
                except Exception as exc:
                    errors.append(exc)
                finally:
                    if done:
                        done.set()

            with patch.object(audio.subprocess, "Popen") as process:
                process.return_value.wait.return_value = 0
                first = threading.Thread(
                    target=speak, args=("First.", cancelled, first_done)
                )
                second = threading.Thread(
                    target=speak, args=("Second.", threading.Event())
                )
                first.start()
                try:
                    self.assertTrue(entered.wait(2))
                    cancelled.set()
                    local.stop()
                    self.assertTrue(first_done.wait(.5))
                    second.start()
                    self.assertFalse(second_request.wait(.2))
                finally:
                    release.set()
                    first.join(3)
                    if second.ident is not None:
                        second.join(3)
            self.assertEqual(errors, [])
            self.assertTrue(second_request.is_set())
            self.assertEqual(requests, ["First.", "Second."])


if __name__ == "__main__":
    unittest.main()
