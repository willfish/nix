"""Voice controller behavior with external terminal/audio adapters replaced."""

import importlib.util
from pathlib import Path
import tempfile
import threading
import array
import math
import wave
import sqlite3
import os
import io
import json
import subprocess
import sys
from contextlib import closing
import unittest
from unittest.mock import patch

MODULE = (
    Path(__file__).resolve().parents[1] / "home/config/voice/codex_voice.py"
)


def speech_wav(samples):
    out = io.BytesIO()
    with wave.open(out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(24000)
        wav.writeframes(samples)
    return out.getvalue()


class Terminal:
    def __init__(self):
        self.alive = True
        self.state = "idle"
        self.text = []
        self.keys = []

    def validate(self, target):
        if not self.alive:
            raise RuntimeError("Selected Codex process has exited")
        if self.state not in ("idle", "done"):
            raise RuntimeError("Codex is " + self.state)

    def insert(self, target, text):
        self.validate(target)
        self.text.append(text)

    def submit(self, target):
        self.validate(target)
        self.keys.append("Enter")


class Capture:
    def __init__(self):
        self.done = threading.Event()
        self.returncode = None

    def wait(self, timeout=None):
        if not self.done.wait(timeout or 3):
            raise RuntimeError("test capture timed out")
        return self.returncode

    def poll(self):
        return self.returncode

    def send_signal(self, sig):
        self.returncode = 0
        self.done.set()

    def terminate(self):
        self.send_signal(None)


class Audio:
    def __init__(self):
        self.spoken = []
        self.stops = 0
        self.silent = False
        self.transcriptions = 0
        self.transcribe_started = threading.Event()
        self.transcribe_release = threading.Event()
        self.transcribe_release.set()

    def start_capture(self, path):
        with wave.open(str(path), "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(16000)
            samples = array.array(
                "h",
                (
                    0 if self.silent else int(4000 * math.sin(i / 10))
                    for i in range(16000)
                ),
            )
            f.writeframes(samples.tobytes())
        self.capture = Capture()
        return self.capture

    def transcribe(self, path):
        self.transcriptions += 1
        self.transcribe_started.set()
        self.transcribe_release.wait(2)
        return "Please explain the failing tests."

    def cue(self, frequency):
        pass

    def stop(self):
        self.stops += 1

    def speak(self, text, cancelled):
        if not cancelled.is_set():
            self.spoken.append(text)


class VoiceTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Voice controller is not implemented")
        spec = importlib.util.spec_from_file_location("codex_voice", MODULE)
        self.voice = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.voice)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.terminal, self.audio = Terminal(), Audio()
        self.app = self.voice.Controller(
            Path(self.tmp.name), self.terminal, self.audio, lambda *a: None
        )
        self.addCleanup(self.app.stop)
        self.target = {
            "pane": "w1:p2",
            "socket": "/tmp/test-herdr.sock",
            "pid": 100,
            "start": "200",
        }
        self.app.register("token-1", self.target)

    def test_service_restart_restores_only_a_live_process_selection(self):
        target = dict(
            self.target,
            pid=os.getpid(),
            start=self.voice.process_start(os.getpid()),
        )
        self.app.register("surviving-token", target)
        restarted = self.voice.Controller(
            Path(self.tmp.name), self.terminal, self.audio, lambda *a: None
        )
        self.assertTrue(restarted.restore())
        self.assertEqual(restarted.status()["pane"], self.target["pane"])
        self.app.register("dead-token", dict(target, start="wrong-start-time"))
        self.assertFalse(restarted.restore())
        self.assertIsNone(restarted.status()["pane"])

    def test_only_cli_root_threads_pass_the_notification_filter(self):
        root = Path(self.tmp.name)
        with closing(sqlite3.connect(root / "state_5.sqlite")) as db:
            db.execute(
                "CREATE TABLE threads (id TEXT PRIMARY KEY, source TEXT)"
            )
            db.executemany(
                "INSERT INTO threads VALUES (?, ?)",
                [("root", "cli"), ("child", '{"subagent":{}}')],
            )
            db.commit()
        self.assertTrue(self.voice.is_cli_thread("root", root))
        self.assertFalse(self.voice.is_cli_thread("child", root))
        self.assertFalse(self.voice.is_cli_thread("absent' OR 1=1 --", root))
        self.assertFalse(self.voice.is_cli_thread("root", root / "missing"))

    def test_restart_preserves_thread_binding_and_completed_turns(self):
        target = dict(
            self.target,
            pid=os.getpid(),
            start=self.voice.process_start(os.getpid()),
        )
        self.app.register("token-1", target)
        self.assertTrue(self.app.notify("token-1", self.event()))
        restarted = self.voice.Controller(
            Path(self.tmp.name), self.terminal, self.audio, lambda *a: None
        )
        self.assertTrue(restarted.restore())
        self.assertFalse(
            restarted.notify("token-1", self.event("another-thread"))
        )
        self.assertFalse(restarted.notify("token-1", self.event()))
        self.assertTrue(
            restarted.notify("token-1", self.event(turn="next-turn"))
        )

    def test_terminal_escape_sequences_are_rejected_before_insertion(self):
        with self.assertRaisesRegex(RuntimeError, "control"):
            self.app.stage("hello\x1b[201~\nexit", "token-1")
        self.assertEqual(self.terminal.text, [])

    def test_old_launcher_exit_does_not_unregister_replacement(self):
        self.app.register("token-2", self.target)
        self.app.unregister("token-1")
        self.assertEqual(self.app.status()["pane"], "w1:p2")
        self.app.unregister("token-2")
        self.assertIsNone(self.app.status()["pane"])

    def test_record_toggle_stages_transcript_without_submitting(self):
        self.app.record()
        self.assertTrue(self.app.status()["recording"])
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(
            self.terminal.text, ["Please explain the failing tests."]
        )
        self.assertEqual(self.terminal.keys, [])
        self.assertFalse(self.app.status()["recording"])
        self.assertFalse(self.app.status()["transcribing"])

    def test_silence_does_not_reach_whisper_or_codex(self):
        self.audio.silent = True
        self.app.record()
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(self.audio.transcriptions, 0)
        self.assertEqual(self.terminal.text, [])

    def test_cancel_during_transcription_discards_late_result(self):
        self.audio.transcribe_release.clear()
        self.app.record()
        self.app.record()
        self.assertTrue(self.audio.transcribe_started.wait(1))
        worker = self.app.worker
        self.app.stop()
        self.audio.transcribe_release.set()
        worker.join(2)
        self.assertEqual(self.terminal.text, [])

    def test_completion_during_recording_does_not_start_playback(self):
        self.app.auto = True
        self.app.record()
        self.app.notify("token-1", self.event())
        self.assertTrue(self.app.status()["recording"])
        self.assertEqual(self.audio.spoken, [])
        with self.assertRaisesRegex(RuntimeError, "recording"):
            self.app.read()

    def event(self, thread="thread-1", turn="turn-1", text="The tests passed."):
        return {
            "type": "agent-turn-complete",
            "thread-id": thread,
            "turn-id": turn,
            "last-assistant-message": text,
        }

    def test_notifications_are_filtered_by_launcher_token_and_thread(self):
        self.assertFalse(self.app.notify("another-launcher", self.event()))
        self.assertIsNone(self.app.status()["reply"])
        self.assertTrue(self.app.notify("token-1", self.event()))
        self.assertFalse(
            self.app.notify("token-1", self.event("another-thread"))
        )
        self.assertEqual(self.app.status()["reply"], "The tests passed.")

    def test_duplicate_completion_is_not_spoken_twice(self):
        self.app.auto = True
        self.app.notify("token-1", self.event())
        self.app.playback.join(1)
        self.app.notify("token-1", self.event())
        self.assertEqual(self.audio.spoken, ["The tests passed."])

    def test_completion_clears_dictation_submitted_with_manual_enter(self):
        self.app.stage("Check the tests", "token-1")
        self.assertFalse(self.app.notify("another-token", self.event()))
        self.assertTrue(self.app.status()["draft"])
        self.assertTrue(self.app.notify("token-1", self.event()))
        with self.assertRaisesRegex(RuntimeError, "dictation"):
            self.app.send()
        self.assertEqual(self.terminal.keys, [])

    def test_read_strips_code_but_keeps_prose_and_link_labels(self):
        self.app.notify(
            "token-1",
            self.event(
                text=(
                    "**Fixed** the [login](https://example.test).\n"
                    "```sh\nrm -rf example\n```\nTests passed."
                )
            ),
        )
        self.app.read()
        self.app.playback.join(1)
        self.assertEqual(self.audio.spoken, ["Fixed the login. Tests passed."])

    def test_new_registration_clears_previous_reply_and_draft(self):
        self.app.stage("hello", "token-1")
        self.app.notify("token-1", self.event())
        self.app.register("token-2", self.target)
        self.assertIsNone(self.app.status()["reply"])
        self.assertFalse(self.app.status()["draft"])
        self.assertFalse(self.app.notify("token-1", self.event()))

    def test_exited_codex_never_receives_dictation_or_enter(self):
        self.terminal.alive = False
        with self.assertRaisesRegex(RuntimeError, "exited"):
            self.app.stage("Check the tests", "token-1")
        with self.assertRaises(RuntimeError):
            self.app.send()
        self.assertEqual(self.terminal.text, [])
        self.assertEqual(self.terminal.keys, [])

    def test_literal_unicode_draft_is_staged_then_submitted_once(self):
        prompt = "Explain `$(touch /tmp/wrong)` and café\nwithout running it."
        self.app.stage(prompt, "token-1")
        self.assertEqual(self.terminal.text, [prompt])
        self.assertEqual(self.terminal.keys, [])
        self.app.send()
        with self.assertRaisesRegex(RuntimeError, "dictation"):
            self.app.send()
        self.assertEqual(self.terminal.keys, ["Enter"])

    def test_speech_request_selects_english_for_the_local_model(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name), {"tts_url": "http://127.0.0.1:8179/speech"}
        )
        with (
            patch.object(
                self.voice.urllib.request,
                "urlopen",
                return_value=io.BytesIO(speech_wav(b"\0\0" * 24)),
            ) as http,
            patch.object(self.voice.subprocess, "Popen"),
        ):
            audio.speak("Hello William.", threading.Event())
        payload = json.loads(http.call_args.args[0].data)
        self.assertEqual(payload.get("language"), "English")
        self.assertEqual(payload["input"], "Hello William.")

    def test_full_reply_is_synthesized_before_one_continuous_playback(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name), {"tts_url": "http://127.0.0.1:8179/speech"}
        )
        samples = [b"\x11\x00" * 240, b"\x22\x00" * 480]
        text = "First " * 40 + "second " * 10
        played = []

        def capture_playback(command, **kwargs):
            self.assertEqual(http.call_count, 2)
            with wave.open(command[1], "rb") as wav:
                self.assertEqual(wav.getframerate(), 24000)
                self.assertEqual(wav.getnchannels(), 1)
                self.assertEqual(wav.getsampwidth(), 2)
                played.append(wav.readframes(wav.getnframes()))
            return player

        with (
            patch.object(
                self.voice.urllib.request,
                "urlopen",
                side_effect=[io.BytesIO(speech_wav(p)) for p in samples],
            ) as http,
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            player = process.return_value
            process.side_effect = capture_playback
            audio.speak(text, threading.Event())
        self.assertEqual(played, [b"".join(samples)])
        player.wait.assert_called_once_with()
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_streaming_prepares_the_next_chunk_while_playing(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name),
            {"tts_url": "http://unused.test", "playback_mode": "streaming"},
        )
        samples = [b"\x11\x00" * 240, b"\x22\x00" * 480]
        writing = threading.Event()
        prepared = threading.Event()
        played = []

        def synthesize(*args, **kwargs):
            index = http.call_count - 1
            if index == 1:
                self.assertTrue(writing.wait(2), "Playback did not start early")
                prepared.set()
            return io.BytesIO(speech_wav(samples[index]))

        def write_pcm(data):
            if not played:
                writing.set()
                self.assertTrue(
                    prepared.wait(2),
                    "Next chunk was not prepared during playback",
                )
            played.append(data)
            return len(data)

        with (
            patch.object(
                self.voice.urllib.request, "urlopen", side_effect=synthesize
            ) as http,
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            player = process.return_value
            player.stdin.write.side_effect = write_pcm
            player.wait.return_value = 0
            audio.speak("First " * 40 + "second " * 10, threading.Event())
        self.assertEqual(played, samples)
        process.assert_called_once()
        self.assertIn("--raw", process.call_args.args[0])
        self.assertEqual(process.call_args.kwargs["stdin"], subprocess.PIPE)
        player.stdin.close.assert_called_once()
        player.wait.assert_called_once_with()
        self.assertIsNone(audio.player)

    def test_streaming_cancellation_discards_the_prefetched_chunk(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name),
            {"tts_url": "http://unused.test", "playback_mode": "streaming"},
        )
        cancelled = threading.Event()
        preparing = threading.Event()
        stopped = threading.Event()
        samples = b"\x11\x00" * 240

        def synthesize(*args, **kwargs):
            if http.call_count == 2:
                preparing.set()
                self.assertTrue(stopped.wait(2))
            return io.BytesIO(speech_wav(samples))

        def write_pcm(data):
            self.assertTrue(preparing.wait(2))
            cancelled.set()
            audio.stop()
            stopped.set()
            return len(data)

        with (
            patch.object(
                self.voice.urllib.request, "urlopen", side_effect=synthesize
            ) as http,
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            player = process.return_value
            player.poll.return_value = None
            player.terminate.side_effect = lambda: setattr(
                player.poll, "return_value", 0
            )
            player.stdin.closed = False
            player.stdin.write.side_effect = write_pcm
            audio.speak("First " * 40 + "second " * 10, cancelled)
        player.stdin.write.assert_called_once_with(samples)
        player.terminate.assert_called_once()
        player.stdin.close.assert_called_once()
        self.assertIsNone(audio.player)

    def test_streaming_failure_stops_playback_and_closes_the_pipe(self):
        samples = b"\x11\x00" * 240
        for failure in (
            TimeoutError("Synthesis timed out"), speech_wav(samples)[:-2]
        ):
            with self.subTest(failure=type(failure).__name__):
                audio = self.voice.LocalAudio(
                    Path(self.tmp.name),
                    {
                        "tts_url": "http://unused.test",
                        "playback_mode": "streaming",
                    },
                )
                with (
                    patch.object(
                        self.voice.urllib.request, "urlopen",
                        side_effect=[
                            io.BytesIO(speech_wav(samples)),
                            failure if isinstance(failure, Exception)
                            else io.BytesIO(failure),
                        ],
                    ),
                    patch.object(self.voice.subprocess, "Popen") as process,
                ):
                    player = process.return_value
                    player.poll.return_value = None
                    player.stdin.closed = False
                    with self.assertRaises((TimeoutError, RuntimeError)):
                        audio.speak(
                            "First " * 40 + "second " * 10, threading.Event()
                        )
                player.terminate.assert_called_once()
                player.wait.assert_called_once_with(timeout=5)
                player.stdin.close.assert_called_once()
                self.assertIsNone(audio.player)

    def test_cancelling_before_first_streamed_chunk_never_starts_playback(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name),
            {"tts_url": "http://unused.test", "playback_mode": "streaming"},
        )
        cancelled = threading.Event()

        def synthesize(*args, **kwargs):
            cancelled.set()
            return io.BytesIO(speech_wav(b"\0\0" * 24))

        with (
            patch.object(
                self.voice.urllib.request, "urlopen", side_effect=synthesize
            ),
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            audio.speak("Hello William.", cancelled)
        process.assert_not_called()

    def test_cancelling_buffered_speech_discards_audio_before_playback(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name), {"tts_url": "http://127.0.0.1:8179/speech"}
        )
        cancelled = threading.Event()

        def synthesize(*args, **kwargs):
            if http.call_count == 2:
                cancelled.set()
            return io.BytesIO(speech_wav(b"\0\0" * 24))

        with (
            patch.object(
                self.voice.urllib.request, "urlopen", side_effect=synthesize
            ) as http,
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            audio.speak("A longer reply. " * 60, cancelled)
        self.assertEqual(http.call_count, 2)
        process.assert_not_called()
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_later_synthesis_failure_never_plays_a_partial_reply(self):
        audio = self.voice.LocalAudio(
            Path(self.tmp.name), {"tts_url": "http://127.0.0.1:8179/speech"}
        )
        with (
            patch.object(
                self.voice.urllib.request,
                "urlopen",
                side_effect=[
                    io.BytesIO(speech_wav(b"\0\0" * 24)),
                    TimeoutError("Synthesis timed out"),
                ],
            ),
            patch.object(self.voice.subprocess, "Popen") as process,
        ):
            with self.assertRaisesRegex(TimeoutError, "Synthesis timed out"):
                audio.speak("A longer reply. " * 60, threading.Event())
        process.assert_not_called()
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_exit_during_synthesis_leaves_no_speech_buffer(self):
        script = """
import importlib.util
import io
import os
from pathlib import Path
import sys
import threading
import wave

spec = importlib.util.spec_from_file_location("voice", sys.argv[1])
voice = importlib.util.module_from_spec(spec)
spec.loader.exec_module(voice)
data = io.BytesIO()
with wave.open(data, "wb") as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(24000)
    wav.writeframes(b"\\0\\0" * 24000)
calls = 0

def synthesize(*args, **kwargs):
    global calls
    calls += 1
    if calls == 2:
        os._exit(0)
    return io.BytesIO(data.getvalue())

voice.urllib.request.urlopen = synthesize
voice.LocalAudio(Path(sys.argv[2]), {"tts_url": "http://unused.test"}).speak(
    "A longer reply. " * 60, threading.Event()
)
raise SystemExit("Did not exit during synthesis")
"""
        subprocess.run(
            [sys.executable, "-c", script, str(MODULE), self.tmp.name],
            check=True,
            timeout=5,
            capture_output=True,
        )
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_invalid_later_wav_discards_the_buffered_reply(self):
        original = speech_wav(b"\0\0" * 24)
        stereo = io.BytesIO()
        with wave.open(stereo, "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(24000)
            wav.writeframes(b"\0\0\0\0" * 24)
        for invalid, error in (
            (stereo.getvalue(), "Speech audio format changed"),
            (original[:-2], "Incomplete speech audio"),
        ):
            with self.subTest(error=error):
                audio = self.voice.LocalAudio(
                    Path(self.tmp.name),
                    {"tts_url": "http://127.0.0.1:8179/speech"},
                )
                with (
                    patch.object(
                        self.voice.urllib.request,
                        "urlopen",
                        side_effect=[io.BytesIO(original), io.BytesIO(invalid)],
                    ),
                    patch.object(self.voice.subprocess, "Popen") as process,
                ):
                    with self.assertRaisesRegex(RuntimeError, error):
                        audio.speak("A longer reply. " * 60, threading.Event())
                process.assert_not_called()
                self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_blocked_agent_does_not_receive_enter(self):
        self.app.stage("Check the tests", "token-1")
        self.terminal.state = "blocked"
        with self.assertRaisesRegex(RuntimeError, "blocked"):
            self.app.send()
        self.assertEqual(self.terminal.keys, [])

    def test_late_transcription_cannot_reach_replacement_session(self):
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        with self.assertRaisesRegex(RuntimeError, "changed"):
            self.app.stage("old session words", "token-1")
        self.assertEqual(self.terminal.text, [])


if __name__ == "__main__":
    unittest.main()
