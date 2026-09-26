"""Voice controller behavior with external terminal/audio adapters replaced."""

import importlib.util
from pathlib import Path
import tempfile
import threading
import array
import math
import wave
import os
import io
import json
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

MODULE = Path(os.environ.get(
    "VOICE_TEST_CONTROLLER",
    Path(__file__).resolve().parents[1] / \
         "home/config/voice/voice_controller.py",
))


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
        self.submitted = []

    def validate_target(self, target):
        if not self.alive:
            raise RuntimeError("Selected Pi process has exited")

    def validate(self, target):
        self.validate_target(target)
        if self.state not in ("idle", "done"):
            raise RuntimeError("Pi is " + self.state)

    def insert(self, target, text):
        self.validate(target)
        self.text.append(text)

    def submit(self, target):
        self.validate(target)
        self.keys.append("Enter")
        self.submitted.append(target["pane"])


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

    def wait_ready(self, timeout=5):
        return True

    def close(self):
        if self.poll() is None:
            self.terminate()

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
        self.stt_backend = "whisper"
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

    def transcribe(self, path, cancelled=None):
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

    def set_stt_backend(self, backend):
        self.stt_backend = backend

    def status(self):
        return {
            "selected_stt": self.stt_backend,
            "stt_backends": {
                "whisper": "Whisper (local GPU)",
                "deepgram": "Deepgram (cloud)",
            },
        }


class VoiceTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Voice controller is not implemented")
        spec = importlib.util.spec_from_file_location(
            "voice_controller", MODULE)
        self.voice = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.voice)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.terminal, self.audio = Terminal(), Audio()
        self.app = self.voice.Controller(
            Path(self.tmp.name), self.terminal, self.audio, lambda *a: None
        )
        def stop_workers():
            self.app.stop()
            if self.app.worker:
                self.app.worker.join(2)

        self.addCleanup(stop_workers)
        self.target = {
            "pane": "w1:p2",
            "socket": "/tmp/test-herdr.sock",
            "pid": 100,
            "start": "200",
        }
        self.app.register("token-1", self.target)

    def test_missing_session_raises_the_card_without_a_transcript(self):
        app = self.voice.Controller(
            Path(self.tmp.name) / "fresh",
            self.terminal,
            self.audio,
            lambda *a: None,
        )
        with self.assertRaisesRegex(RuntimeError, "Select a Pi voice session"):
            app.interact()
        status = app.status()
        self.assertTrue(status["osd"])
        self.assertEqual(
            status["osd_message"], "Select a Pi voice session first"
        )

    def test_notice_stays_on_the_card_instead_of_a_desktop_popup(self):
        called = []
        app = self.voice.Controller(
            Path(self.tmp.name) / "notice",
            self.terminal,
            self.audio,
            lambda *args: called.append(args),
        )
        app.notice("Could not read the reply", "engine busy", tone="red")
        status = app.status()
        self.assertFalse(called)
        self.assertTrue(status["osd"])
        self.assertIn("engine busy", status["osd_message"])
        self.assertEqual(status["osd_tone"], "red")
        self.assertNotIn("notify", status["osd_message"])

    def start_recording(self):
        self.app.record()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if self.app.status()["recording"]:
                return
            if self.app.status()["phase"] == "error":
                break
            time.sleep(0.005)
        self.fail(f"Capture did not become ready: {self.app.status()}")

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

    def test_restart_adopts_resident_engine_and_last_restored_session_stops_it(
        self,
    ):
        from voice_engines import EngineManager
        from test_voice_engines import Queue
        queue, commands = Queue(), []
        manager = EngineManager(
            readiness=lambda *a: self.fail('restart must not warm engines'),
            runner=lambda engine, command, timeout: commands.append(
                (engine, command)),
            scheduler=queue, clock=lambda: 10000)
        self.addCleanup(manager.close)
        self.app.register(
            'survivor',
            dict(
                self.target,
                pid=os.getpid(),
                start=self.voice.process_start(os.getpid()),
            ),
        )
        restarted = self.voice.Controller(
            Path(self.tmp.name), self.terminal, self.audio, lambda *a: None,
            engines=manager)
        self.assertTrue(restarted.restore())
        manager.reconcile_startup(lambda engine, timeout: engine == 'stt')
        queue.drain()
        self.assertEqual(commands, [])
        self.assertTrue(manager.state('stt').running)
        restarted.unregister('survivor')
        queue.drain()
        self.assertEqual(commands, [('stt', 'stop')])

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

    def test_registered_sessions_can_be_selected_without_losing_dictation(self):
        self.app.pending = "Remember this request"
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        self.assertEqual(len(self.app.status()["sessions"]), 2)
        self.app.select("token-1")
        self.assertEqual(self.app.pending, "Remember this request")
        self.assertEqual(self.app.status()["pane"], "w1:p2")
        self.app.unregister("token-2")
        self.assertEqual(self.app.status()["pane"], "w1:p2")

    def test_session_menu_labels_use_harness_and_conversation_or_pane_id(self):
        self.app.register("token-1", self.target, "conversation-one")
        self.app.register(
            "token-2", dict(self.target, harness="qwen-pi", pane="w1:p3")
        )
        self.app.register(
            "token-3", dict(self.target, harness="pi", pane="w1:p4"),
            "conversation-pi",
        )
        self.assertEqual(
            [(s["label"], s["selected"])
             for s in self.app.status()["sessions"]],
            [("pi: w1:p2", False), ("qwen-pi: w1:p3", False),
             ("pi: w1:p4", True)],
        )

    def test_harness_session_change_requires_explicit_rebind(self):
        self.app.register("pi-token", dict(self.target, harness="pi"))
        self.app.harness_event("pi-token", {
            "harness": "pi", "type": "session", "session": "pi-first",
        })
        self.app.harness_event("pi-token", {
            "harness": "pi", "type": "session", "session": "pi-next",
        })
        self.assertEqual(self.app.thread, "pi-first")
        self.app.rebind()
        self.assertEqual(self.app.thread, "pi-next")
        self.assertEqual(self.app.target["session"], "pi-next")

    def test_launcher_keeps_wrappers_and_uses_installed_pi_extension_once(self):
        for harness in ("pi", "qwen-pi"):
            with patch.object(
                self.voice, 'installed_pi_extension'
            ) as installed:
                command = self.voice.launcher_command(
                    harness, ["--continue"], Path(self.tmp.name), "notify"
                )
            installed.assert_called_once_with()
            self.assertEqual(command, [harness, '--continue'])
            with self.assertRaisesRegex(RuntimeError, "interactive"):
                self.voice.launcher_command(
                    harness, ["--mode", "rpc"], Path(self.tmp.name), "notify"
                )

    def test_launcher_rejects_missing_or_disabled_pi_bridge(self):
        with patch.object(
            self.voice.Path, 'home', return_value=Path(self.tmp.name)
        ):
            with self.assertRaisesRegex(RuntimeError, 'hmswitch'):
                self.voice.launcher_command(
                    'pi', [], Path(self.tmp.name), 'notify')
        for flag in ('--no-extensions', '-ne'):
            with self.assertRaisesRegex(RuntimeError, 'requires.*extension'):
                self.voice.launcher_command(
                    'pi', [flag], Path(self.tmp.name), 'notify')

    def test_failed_launcher_cleans_up_without_stopping_other_sessions(self):
        for sessions in ([], [{"token": "another-session"}]):
            with (
                self.subTest(sessions=sessions),
                patch.dict(
                    os.environ,
                    {
                        "HERDR_ENV": "1",
                        "HERDR_PANE_ID": "w1:p2",
                        "HERDR_SOCKET_PATH": "/tmp/herdr.sock",
                    },
                ),
                patch.object(
                    self.voice, "runtime_dir", return_value=Path(self.tmp.name)
                ),
                patch.object(
                    self.voice, "call", return_value={"sessions": sessions}
                ),
                patch.object(self.voice, 'installed_pi_extension'),
                patch.object(self.voice.subprocess, "run") as service_commands,
                patch.object(
                    self.voice.subprocess,
                    "Popen",
                    side_effect=FileNotFoundError("Missing harness"),
                ),
            ):
                with self.assertRaises(FileNotFoundError):
                    self.voice.launch([], "pi")
                service_commands.assert_not_called()
                directories = [
                    path for path in Path(self.tmp.name).iterdir()
                    if path.is_dir()
                ]
                self.assertEqual(directories, [])

    def test_late_uncertain_paste_does_not_clear_another_sessions_words(self):
        entered, release = threading.Event(), threading.Event()

        def insert(_target, _text):
            entered.set()
            release.wait(2)
            raise self.voice.DeliveryUncertain("May have arrived")

        with patch.object(self.terminal, "insert", side_effect=insert):
            self.start_recording()
            self.app.record()
            self.assertTrue(entered.wait(1))
            worker = self.app.worker
            self.app.register("new-token", dict(self.target, pane="w1:p3"))
            self.app.pending = "New session words"
            release.set()
            worker.join(2)
        self.assertEqual(self.app.pending, "New session words")

    def test_rebind_cannot_accept_delayed_old_conversation_reply(self):
        self.app.harness_event("token-1", {
            "harness": "pi", "type": "session", "session": "thread-1",
        })
        self.app.harness_event("token-1", {
            "harness": "pi", "type": "session",
            "session": "new-conversation",
        })
        self.app.rebind()
        self.assertFalse(self.app.notify(
            "token-1", self.event(turn="late-old-turn")
        ))
        self.assertTrue(self.app.notify(
            "token-1", self.event(thread="new-conversation", turn="new-turn")
        ))

    def test_dictate_selects_this_session_and_does_not_send(self):
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        self.assertEqual(self.app.token, "token-2")
        self.voice.dispatch(self.app, {"action": "dictate", "token": "token-1"})
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if self.app.status()["recording"]:
                break
            time.sleep(0.005)
        else:
            self.fail(f"Capture did not become ready: {self.app.status()}")
        self.assertEqual(self.app.token, "token-1")
        self.voice.dispatch(self.app, {"action": "dictate", "token": "token-1"})
        self.app.worker.join(2)
        self.assertEqual(self.terminal.keys, [])
        self.assertTrue(self.app.status()["draft"])

    def test_dictate_cancel_discards_recording(self):
        self.start_recording()
        self.voice.dispatch(
            self.app, {"action": "dictate-cancel", "token": "token-1"}
        )
        self.app.worker.join(2)
        self.assertFalse(self.app.status()["recording"])
        self.assertEqual(self.terminal.text, [])
        self.assertEqual(self.terminal.keys, [])

    def test_stt_backend_toggle_does_not_start_whisper(self):
        self.voice.dispatch(self.app, {"action": "stt:deepgram"})
        self.assertEqual(self.app.audio.stt_backend, "deepgram")
        self.assertEqual(self.app.status()["selected_stt"], "deepgram")

    def test_stt_backend_toggle_is_blocked_while_recording(self):
        self.start_recording()
        with self.assertRaisesRegex(RuntimeError, "before changing dictation"):
            self.voice.dispatch(self.app, {"action": "stt:deepgram"})
        self.assertEqual(self.app.audio.stt_backend, "whisper")
        self.voice.dispatch(self.app, {"action": "stop"})
        self.app.worker.join(2)

    def test_dictate_requires_a_bound_session_token(self):
        with self.assertRaisesRegex(RuntimeError, "not bound"):
            self.voice.dispatch(self.app, {"action": "dictate"})

    def test_record_toggle_stages_transcript_without_submitting(self):
        self.start_recording()
        self.assertTrue(self.app.status()["recording"])
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(
            self.terminal.text, ["Please explain the failing tests."]
        )
        self.assertEqual(self.terminal.keys, [])
        self.assertFalse(self.app.status()["recording"])
        self.assertFalse(self.app.status()["transcribing"])

    def test_finished_chunks_are_staged_while_recording(self):
        class LiveCapture(Capture):
            def __init__(self):
                super().__init__()
                self._chunks = []
                self._progress = threading.Event()

            def drain_chunks(self):
                chunks, self._chunks = self._chunks, []
                if not chunks and self.poll() is None:
                    self._progress.clear()
                return chunks

            def wait_progress(self, timeout=None):
                if self.done.is_set():
                    self._progress.set()
                    return True
                return self._progress.wait(timeout)

            def emit(self, samples):
                self._chunks.append(samples)
                self._progress.set()

        live = LiveCapture()
        texts = iter(["First slice.", "Second slice."])
        voiced = array.array(
            "h", (int(4000 * math.sin(i / 10)) for i in range(16000))
        )

        def start_capture(path):
            with wave.open(str(path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(b"\0\0" * 1600)
            self.audio.capture = live
            return live

        def transcribe(path, cancelled=None):
            self.audio.transcriptions += 1
            return next(texts)

        with (
            patch.object(
                self.audio, "start_capture", side_effect=start_capture
            ),
            patch.object(self.audio, "transcribe", side_effect=transcribe),
        ):
            self.app.record()
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                if self.app.status()["recording"]:
                    break
                time.sleep(0.005)
            else:
                self.fail(f"Capture did not become ready: {self.app.status()}")
            live.emit(voiced)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if self.terminal.text:
                    break
                time.sleep(0.01)
            self.assertEqual(self.terminal.text, ["First slice."])
            self.assertTrue(self.app.status()["recording"])
            live.emit(voiced)
            live.returncode = 0
            live.done.set()
            live._progress.set()
            self.app.worker.join(2)
        self.assertEqual(
            self.terminal.text, ["First slice.", "Second slice."]
        )
        self.assertFalse(self.app.status()["recording"])
        self.assertTrue(self.app.status()["draft"])

    def test_primary_hotkey_records_then_transcribes_then_sends(self):
        def press():
            return self.voice.dispatch(self.app, {"action": "interact"})

        press()
        deadline = time.monotonic() + 1
        while not self.app.status()["recording"]:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        press()
        self.app.worker.join(2)
        self.assertEqual(self.terminal.keys, [])
        self.assertTrue(self.app.status()["draft"])
        with patch.object(self.audio, "start_capture") as capture:
            press()
            capture.assert_not_called()
        self.assertEqual(self.terminal.keys, ["Enter"])
        self.assertFalse(self.app.status()["draft"])

    def test_primary_hotkey_sends_retained_words_when_agent_is_ready(self):
        self.terminal.state = "working"
        self.start_recording()
        self.app.interact()
        self.app.worker.join(2)
        self.app.interact()
        self.assertTrue(self.app.status()["pending"])
        self.assertFalse(self.app.status()["recording"])
        self.assertTrue(self.app.send_when_idle)
        self.terminal.state = "idle"
        self.app._flush_queued_send()
        self.assertEqual(self.terminal.text, [
            "Please explain the failing tests."
        ])
        self.assertEqual(self.terminal.keys, ["Enter"])

    def test_busy_event_keeps_draft_until_send(self):
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertTrue(self.app.status()["draft"])
        self.app.harness_event("token-1", {
            "harness": "pi", "type": "session", "session": "thread-1",
        })
        self.assertTrue(self.app.harness_event("token-1", {
            "harness": "pi", "type": "busy", "session": "thread-1",
        }))
        self.assertTrue(self.app.status()["draft"])
        self.assertEqual(self.terminal.keys, [])

    def test_primary_hotkey_does_not_send_while_transcription_is_pending(self):
        self.audio.transcribe_release.clear()
        self.start_recording()
        self.app.interact()
        self.assertTrue(self.audio.transcribe_started.wait(1))
        with self.assertRaisesRegex(RuntimeError, "transcribing"):
            self.app.interact()
        self.assertEqual(self.terminal.keys, [])
        self.audio.transcribe_release.set()
        self.app.worker.join(2)
        self.assertTrue(self.app.status()["draft"])

    def test_primary_hotkey_never_fans_out_to_other_registered_sessions(self):
        self.app.stage("First session request", "token-1")
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        self.app.stage("Second session request", "token-2")
        self.app.interact()
        self.assertEqual(self.terminal.submitted, ["w1:p3"])
        self.app.select("token-1")
        self.assertTrue(self.app.status()["draft"])
        self.app.interact()
        self.assertEqual(self.terminal.submitted, ["w1:p3", "w1:p2"])

    def test_cancel_between_hotkey_selection_and_send_prevents_enter(self):
        self.app.stage("Review this", "token-1")
        original = self.app.send

        def delayed_send(**kwargs):
            self.app.stop()
            original(**kwargs)

        with patch.object(self.app, "send", side_effect=delayed_send):
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                self.app.interact()
        self.assertEqual(self.terminal.keys, [])

    def test_cancel_after_retained_paste_prevents_hotkey_submission(self):
        self.app.pending = "Retained dictation"
        original = self.app.stage

        def stage_then_cancel(*args, **kwargs):
            result = original(*args, **kwargs)
            self.app.stop()
            return result

        with patch.object(self.app, "stage", side_effect=stage_then_cancel):
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                self.app.interact()
        self.assertEqual(self.terminal.text, ["Retained dictation"])
        self.assertEqual(self.terminal.keys, [])
        self.assertTrue(self.app.status()["draft"])

    def test_hotkey_does_not_follow_selection_into_another_draft(self):
        self.app.stage("First request", "token-1")
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        self.app.stage("Second request", "token-2")
        self.app.select("token-1")
        original = self.app.send

        def select_then_send(**kwargs):
            self.app.select("token-2")
            original(**kwargs)

        with patch.object(self.app, "send", side_effect=select_then_send):
            with self.assertRaisesRegex(RuntimeError, "session changed"):
                self.app.interact()
        self.assertEqual(self.terminal.submitted, [])
        self.assertTrue(self.app.status()["draft"])

    def test_overlapping_hotkeys_submit_the_same_draft_only_once(self):
        self.app.stage("Review this", "token-1")
        lock = self.app.delivery_lock
        original_validate = self.terminal.validate
        first_validate = threading.Event()
        second_acquire = threading.Event()
        first_done = threading.Event()
        errors = []

        class SchedulingLock:
            def acquire(self, blocking=False):
                if threading.current_thread().name == "second-hotkey":
                    second_acquire.set()
                    first_done.wait(2)
                return lock.acquire(blocking=blocking)

            def release(self):
                lock.release()

        def validate(target):
            if threading.current_thread().name == "first-hotkey":
                first_validate.set()
                second_acquire.wait(2)
            original_validate(target)

        def press():
            try:
                self.app.interact()
            except RuntimeError as exc:
                errors.append(str(exc))
            finally:
                if threading.current_thread().name == "first-hotkey":
                    first_done.set()

        self.app.delivery_lock = SchedulingLock()
        with patch.object(self.terminal, "validate", side_effect=validate):
            first = threading.Thread(target=press, name="first-hotkey")
            second = threading.Thread(target=press, name="second-hotkey")
            first.start()
            self.assertTrue(first_validate.wait(1))
            second.start()
            first.join(3)
            second.join(3)
        self.assertFalse(first.is_alive() or second.is_alive())
        self.assertEqual(self.terminal.keys, ["Enter"])
        self.assertEqual(errors, ["No new dictation to send"])

    def test_failed_sound_cue_does_not_prevent_dictation(self):
        def failed_cue(_frequency):
            raise subprocess.TimeoutExpired("pw-play", 3)

        self.audio.cue = failed_cue
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(len(self.terminal.text), 1)

    def test_busy_capable_terminal_stages_without_automatic_submission(self):
        self.terminal.state = "working"
        with patch.object(
            self.terminal, "validate",
            side_effect=self.terminal.validate_target,
        ):
            self.start_recording()
            self.app.record()
            self.app.worker.join(2)
            self.assertFalse(self.app.worker.is_alive())
            self.assertTrue(self.app.status()["draft"])
            self.assertFalse(self.app.status()["pending"])
            self.assertFalse(self.app.send_when_idle)
            self.assertEqual(len(self.terminal.text), 1)
            self.assertEqual(self.terminal.keys, [])
            self.app._flush_queued_send()
            self.assertEqual(self.terminal.keys, [])
            self.app.interact()
            self.assertEqual(self.terminal.keys, ["Enter"])

    def test_busy_legacy_terminal_never_auto_sends_after_recording(self):
        self.terminal.state = "working"
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertTrue(self.app.status()["pending"])
        self.assertFalse(self.app.send_when_idle)
        self.terminal.state = "idle"
        self.app._flush_queued_send()
        self.assertEqual(self.terminal.text, [])
        self.assertEqual(self.terminal.keys, [])
        self.app.send()
        self.assertEqual(self.terminal.keys, ["Enter"])

    def test_busy_terminal_does_not_block_capture_or_lose_transcript(self):
        self.terminal.state = "unknown"
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertTrue(self.app.status()["pending"])
        self.assertEqual(self.terminal.text, [])
        self.terminal.state = "idle"
        self.app.send()
        self.assertEqual(len(self.terminal.text), 1)
        self.assertEqual(self.terminal.keys, ["Enter"])

    def test_cancel_is_immediate_during_paste_and_disarms_late_result(self):
        entered, release, stopped = (threading.Event() for _ in range(3))

        def insert(_target, _text):
            entered.set()
            release.wait(2)

        with patch.object(self.terminal, "insert", side_effect=insert):
            self.start_recording()
            self.app.record()
            self.assertTrue(entered.wait(1))
            stopper = threading.Thread(
                target=lambda: (self.app.stop(), stopped.set()), daemon=True
            )
            stopper.start()
            try:
                self.assertTrue(stopped.wait(0.2), "Cancel waited for paste")
            finally:
                release.set()
                stopper.join(2)
                self.app.worker.join(2)
        self.assertFalse(self.app.status()["draft"])
        self.assertFalse(self.app.status()["pending"])

    def test_paste_connect_failure_is_known_not_to_have_delivered(self):
        terminal = self.voice.Herdr()
        with patch.object(self.voice.socket, "socket") as connection:
            sock = connection.return_value.__enter__.return_value
            sock.connect.side_effect = ConnectionRefusedError()
            with self.assertRaises(RuntimeError):
                terminal.input_request(self.target, "Retain this speech")
            sock.sendall.assert_not_called()

    def test_paste_timeout_is_ambiguous_and_never_retried(self):
        terminal = self.voice.Herdr()
        with patch.object(self.voice.socket, "socket") as connection:
            sock = connection.return_value.__enter__.return_value
            incoming = sock.makefile.return_value.__enter__.return_value
            incoming.readline.side_effect = TimeoutError()
            with self.assertRaises(self.voice.DeliveryUncertain):
                terminal.input_request(self.target, "May have arrived")
            self.assertEqual(sock.sendall.call_count, 1)
        with patch.object(
            self.terminal, "insert",
            side_effect=self.voice.DeliveryUncertain("May have arrived"),
        ):
            self.start_recording()
            self.app.record()
            self.app.worker.join(1)
        self.assertFalse(self.app.status()["pending"])
        self.assertFalse(self.app.status()["draft"])
        with self.assertRaisesRegex(RuntimeError, "dictation"):
            self.app.send()

    def test_read_only_terminal_timeout_is_a_retryable_failure(self):
        with patch.object(
            self.voice.subprocess, "run",
            side_effect=subprocess.TimeoutExpired("herdr", 8),
        ):
            with self.assertRaises(RuntimeError):
                self.voice.Herdr().request(self.target, "agent", "get")

    def test_uncertain_retained_paste_disarms_further_send_attempts(self):
        self.app.pending = "Retained speech"
        self.app.phase = "draft"
        with patch.object(
            self.terminal, "insert",
            side_effect=self.voice.DeliveryUncertain("May have arrived"),
        ) as paste:
            with self.assertRaises(self.voice.DeliveryUncertain):
                self.app.send()
            self.assertFalse(self.app.status()["pending"])
            self.assertFalse(self.app.status()["draft"])
            with self.assertRaisesRegex(RuntimeError, "dictation"):
                self.app.send()
            self.assertEqual(paste.call_count, 1)
        self.assertEqual(self.terminal.keys, [])

    def test_failed_capture_is_never_transcribed(self):
        self.start_recording()
        self.audio.capture.returncode = 1
        self.audio.capture.done.set()
        self.app.worker.join(2)
        self.assertEqual(self.audio.transcriptions, 0)
        self.assertEqual(self.app.status()["phase"], "error")

    def test_cancel_discards_retained_dictation(self):
        self.app.pending = "Retained speech"
        self.app.phase = "draft"
        self.app.stop()
        self.assertFalse(self.app.status()["pending"])
        with self.assertRaisesRegex(RuntimeError, "dictation"):
            self.app.send()

    def test_new_recording_appends_retained_dictation(self):
        self.app.pending = "First instruction."
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(self.terminal.text, [
            "First instruction.\nPlease explain the failing tests."
        ])

    def test_silent_replacement_keeps_retained_dictation(self):
        self.app.pending = "Keep this instruction."
        self.audio.silent = True
        self.app.record(mode="replace")
        deadline = time.monotonic() + 1
        while not self.app.status()["recording"]:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(self.app.pending, "Keep this instruction.")
        self.assertEqual(self.terminal.text, [])

    def test_failed_transcription_can_retry_original_recording(self):
        original = self.audio.transcribe
        self.audio.transcribe = lambda *args: (_ for _ in ()).throw(
            RuntimeError("Whisper unavailable")
        )
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertTrue(self.app.status()["retry"])
        self.assertEqual(len(list(Path(self.tmp.name).glob("*.wav"))), 1)
        self.audio.transcribe = original
        self.app.retry()
        self.app.worker.join(2)
        self.assertEqual(len(self.terminal.text), 1)
        self.assertFalse(self.app.status()["retry"])
        self.assertIsNone(self.app.active_retry)
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])

    def test_cancelled_staged_draft_can_be_explicitly_sent_once(self):
        self.app.stage("Review this", "token-1")
        self.app.stop()
        self.app.send()
        self.assertEqual(self.terminal.keys, ["Enter"])
        with self.assertRaises(RuntimeError):
            self.app.send()

    def test_failed_retries_keep_original_expiry(self):
        with patch.object(
            self.audio, "transcribe", side_effect=RuntimeError("Unavailable")
        ):
            self.start_recording()
            self.app.record()
            self.app.worker.join(2)
            path, _, _, expiry, _, _ = self.app.retry_audio
            self.app.retry()
            self.app.worker.join(2)
            self.assertEqual(self.app.retry_audio[3], expiry)
            self.assertTrue(path.exists())
            with patch.object(
                self.voice.time, "monotonic", return_value=expiry + 1
            ):
                with self.app.lock:
                    self.app._expire_retry()  # Maintenance, never status I/O.
                self.assertFalse(self.app.status()["retry"])
            self.assertFalse(path.exists())

    def test_retry_expiry_cancels_inflight_transcription_and_removes_wav(self):
        with patch.object(
            self.audio, "transcribe", side_effect=RuntimeError("Unavailable")
        ):
            self.start_recording()
            self.app.record()
            self.app.worker.join(2)
        path, _, _, expiry, _, _ = self.app.retry_audio
        self.audio.transcribe_release.clear()
        self.audio.transcribe_started.clear()
        self.app.retry()
        self.assertTrue(self.audio.transcribe_started.wait(1))
        with patch.object(
            self.voice.time, "monotonic", return_value=expiry + 1
        ):
            with self.app.lock:
                self.app._expire_retry()
            self.assertEqual(self.app.status()["phase"], "error")
        self.assertFalse(path.exists())
        self.assertTrue(self.app.record_cancelled.is_set())
        self.audio.transcribe_release.set()
        self.app.worker.join(2)
        self.assertEqual(self.terminal.text, [])

    def test_read_does_not_discard_retained_dictation(self):
        self.app.pending = "Retained speech"
        self.app.reply = "Previous reply"
        with self.assertRaisesRegex(RuntimeError, "dictation"):
            self.app.read()
        self.assertTrue(self.app.status()["pending"])

    def test_startup_failure_cleans_up_and_allows_retry(self):
        with patch.object(
            self.audio, "start_capture", side_effect=OSError("No microphone")
        ):
            self.app.record()
            self.app.worker.join(1)
        self.assertEqual(self.app.status()["phase"], "error")
        self.assertEqual(list(Path(self.tmp.name).glob("*.wav")), [])
        self.start_recording()
        self.app.stop()
        self.app.worker.join(1)
        self.assertEqual(self.terminal.text, [])

    def test_cancel_startup_never_opens_the_microphone_afterwards(self):
        validating, release = threading.Event(), threading.Event()

        def validate(_target):
            validating.set()
            release.wait(1)

        with (
            patch.object(
                self.terminal, "validate_target", side_effect=validate
            ),
            patch.object(self.audio, "start_capture") as capture,
        ):
            self.app.record()
            self.assertTrue(validating.wait(1))
            self.assertEqual(self.app.status()["phase"], "starting")
            self.app.record()
            release.set()
            self.app.worker.join(1)
            capture.assert_not_called()
        self.assertFalse(self.app.status()["recording"])

    def test_cancelled_worker_cannot_clear_a_new_recording(self):
        self.audio.transcribe_release.clear()
        self.start_recording()
        self.app.record()
        self.assertTrue(self.audio.transcribe_started.wait(1))
        previous = self.app.worker
        self.app.stop()
        self.start_recording()
        self.audio.transcribe_release.set()
        previous.join(1)
        self.assertTrue(self.app.status()["recording"])
        self.app.stop()
        self.app.worker.join(1)
        self.assertEqual(self.terminal.text, [])

    def test_silence_does_not_reach_whisper_or_the_session(self):
        self.audio.silent = True
        self.start_recording()
        self.app.record()
        self.app.worker.join(2)
        self.assertEqual(self.audio.transcriptions, 0)
        self.assertEqual(self.terminal.text, [])

    def test_non_speech_results_disarm_send_without_pasting(self):
        for text in ("", " \n", "[BLANK_AUDIO]", "(silence)",
                     "[MUSIC] ...", "...", "\u200b\ufeff"):
            with self.subTest(text=text):
                self.app.draft = True
                self.app.stage(text, "token-1")
                self.assertFalse(self.app.status()["draft"])
                with self.assertRaisesRegex(RuntimeError, "dictation"):
                    self.app.send()
        self.assertEqual(self.terminal.text, [])
        self.assertEqual(self.terminal.keys, [])

    def test_non_speech_markers_are_removed_from_real_dictation(self):
        self.app.stage("[BLANK_AUDIO] Check the tests. [MUSIC]", "token-1")
        self.assertEqual(self.terminal.text, ["Check the tests."])

    def test_real_audio_drained_transcript_recovery(self):
        from voice_audio import LocalAudio
        from voice_engines import EngineManager
        for action, previous, expected in (
            ('lost', None, 'Late words'),
            ('lost', 'Previous words', 'Previous words\nLate words'),
            ('returned', None, 'Late words'),
            ('contended', None, 'Late words'),
            ('completed', 'Previous words', 'Previous words\nLate words'),
            ('stop', None, None),
            ('discard', None, None),
            ('revision', None, None),
            ('shutdown', None, None),
        ):
            with self.subTest(action=action, previous=previous):
                self.app.recover_discard()
                self.app.register('token-1', dict(self.target, harness='pi'))
                audio = LocalAudio(Path(self.tmp.name), {
                                   'stt_url': 'http://test/stt'})
                manager = EngineManager(
                    runner=lambda *a: None, readiness=lambda *a: True)
                self.addCleanup(manager.close)
                audio.engines = manager
                audio.start_capture = self.audio.start_capture
                audio.cue = lambda *a: None
                self.app.audio = audio
                self.app.engines = manager
                self.app.pending = previous
                entered, release, drained = (
                    threading.Event() for _ in range(3))
                original_transcribe = audio.transcribe_drained
                def transcribe(path, cancelled, callback):
                    def completed(text):
                        try:
                            if action == 'completed':
                                entered.set()
                                self.assertTrue(release.wait(2))
                            # The real adapter must release both locks first.
                            self.assertTrue(audio.lock.acquire(blocking=False))
                            audio.lock.release()
                            self.assertTrue(
                                audio.recognition_lock.acquire(blocking=False))
                            audio.recognition_lock.release()
                            callback(text)
                        finally:
                            drained.set()
                    result = original_transcribe(path, cancelled, completed)
                    if action == 'returned':
                        entered.set()
                        self.assertTrue(release.wait(2))
                    return result
                audio.transcribe_drained = transcribe
                def http(*args, **kwargs):
                    if action not in ('returned', 'completed'):
                        entered.set()
                        self.assertTrue(release.wait(2))
                    return io.BytesIO(b'{"text": "Late words"}')
                # Lease count is the drain fence, not the cancelled controller
                # worker.
                with (
                    patch(
                        'voice_audio.urllib.request.urlopen', side_effect=http
                    ),
                    patch.object(
                        self.app,
                        'retain_dictation',
                        wraps=self.app.retain_dictation,
                    ) as retain,
                ):
                    self.start_recording()
                    self.app.record()
                    self.assertTrue(entered.wait(1))
                    if action == 'stop':
                        self.app.stop()
                    else:
                        with self.app.lock:
                            self.app._remove_session('token-1')
                        if action == 'discard':
                            self.app.recover_discard()
                        elif action == 'revision':
                            self.app.recovery_revision += 1
                        elif action == 'shutdown':
                            self.app.attachments_closed = True
                    if action != 'returned':
                        self.app.worker.join(1)
                        self.assertFalse(self.app.worker.is_alive())
                    self.assertEqual(manager.state('stt').users,
                                     0 if action == 'completed' else 1)
                    if action == 'contended':
                        with self.app.lock:
                            release.set()
                            self.assertFalse(drained.wait(.3))
                        self.assertTrue(drained.wait(1))
                    elif action == 'shutdown':
                        # Shutdown need not release its controller lock to let
                        # a late callback finish; there is no audio-lock cycle.
                        with self.app.lock:
                            release.set()
                            self.assertTrue(drained.wait(.75))
                    else:
                        release.set()
                        self.assertTrue(drained.wait(1))
                    self.app.worker.join(1)
                    attempts = [
                        call
                        for call in retain.call_args_list
                        if call.args[0] and 'Late words' in call.args[0]
                    ]
                    self.assertEqual(
                        len(attempts),
                        1 if expected or action == 'revision' else 0,
                    )
                self.app.attachments_closed = False
                retained = self.app.retained_dictation
                self.assertEqual(retained.text if retained else None, expected)
                self.assertEqual(manager.state('stt').users, 0)
                self.assertEqual(self.terminal.text, [])
                self.assertEqual(self.terminal.keys, [])
                self.app.audio = self.audio
                self.app.engines = None

    def test_cancel_during_transcription_discards_late_result(self):
        self.audio.transcribe_release.clear()
        self.start_recording()
        self.app.record()
        self.assertTrue(self.audio.transcribe_started.wait(1))
        worker = self.app.worker
        self.app.stop()
        self.audio.transcribe_release.set()
        worker.join(2)
        self.assertEqual(self.terminal.text, [])

    def test_completion_during_recording_does_not_start_playback(self):
        self.app.auto = True
        self.start_recording()
        self.app.notify("token-1", self.event())
        self.assertTrue(self.app.status()["recording"])
        self.assertEqual(self.audio.spoken, [])
        with self.assertRaisesRegex(RuntimeError, "recording"):
            self.app.read()

    def event(self, thread="thread-1", turn="turn-1",
              text="Detailed results.\n\n## Spoken summary\nThe tests passed."):
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
                    "Screen-only details.\n\n## Spoken summary\n"
                    "**Fixed** the [login](https://example.test).\n"
                    "```sh\nrm -rf example\n```\nTests passed."
                )
            ),
        )
        self.app.read()
        self.app.playback.join(1)
        self.assertEqual(self.audio.spoken, ["Fixed the login. Tests passed."])

    def test_spoken_summary_formats_and_markdown_cleanup(self):
        for label in ("## Spoken summary", "Spoken summary:",
                      "**Spoken summary:**", "## Summary", "Summary:",
                      "**Summary:**", "### TL;DR", "**TL;DR**:",
                      "TLDR:", "tl;dr:"):
            for separator in ("\n", " ") if label.endswith(":") else ("\n",):
                with self.subTest(label=label, separator=separator):
                    text = ("# Full response\nDo not speak these details.\n\n"
                            + label + separator + "It worked. Next, review it.")
                    self.assertEqual(self.voice.spoken_text(text),
                                     "It worked. Next, review it.")
        self.assertEqual(self.voice.spoken_text(
            "## Spoken summary\n- **Fixed** the issue.\n2. Tests passed."
        ), "Fixed the issue. Tests passed.")

    def test_missing_or_invalid_summaries_fail_closed(self):
        for text in (None, {}, "", "Just read the source.",
                     "# Overview\nNot an explicit summary.",
                     "Spoken summary is a feature, not a label.",
                     "Summary of the work is not a label.",
                     "```md\n## Spoken summary\nExample only.\n```",
                     "~~~~md\n## Spoken summary\nExample only.\n~~~~",
                     "> ## Spoken summary\n> A quoted example.",
                     "## Spoken summary\n",
                     "## Spoken summary\n```sh\nexit\n```",
                     "## Spoken summary\nShort.\n## Details\nLong answer.",
                     "## Spoken summary\nShort.\nDetails\n-------\nDetails.",
                     "## Spoken summary\n" + "word " * 121,
                     "## Spoken summary\n" + "a" * 1501):
            with self.subTest(text=str(text)[:100]):
                self.assertEqual(self.voice.spoken_text(text), "")

    def test_final_summary_wins_over_earlier_sections_and_fenced_examples(self):
        self.assertEqual(self.voice.spoken_text(
            "## TL;DR\nEarlier overview.\n## Details\nScreen only.\n"
            "````md\n```\n## Spoken summary\nFake.\n```\n````\n"
            "## Spoken summary\nThe final result."
        ), "The final result.")

    def test_missing_summary_clears_previous_reply_and_never_autoplays(self):
        self.app.notify("token-1", self.event())
        self.app.auto = True
        self.app.notify("token-1", self.event(turn="next", text="No summary."))
        self.assertFalse(self.app.reply)
        self.assertEqual(self.audio.spoken, [])
        with self.assertRaisesRegex(RuntimeError, "No summary"):
            self.app.read()

    def test_unselected_session_stores_only_summary_for_replay(self):
        self.app.register("token-2", dict(self.target, pane="w1:p3"))
        self.app.notify("token-1", self.event())
        self.app.select("token-1")
        self.app.read()
        self.app.playback.join(1)
        self.assertEqual(self.audio.spoken, ["The tests passed."])

    def test_pi_profiles_share_summary_only_playback(self):
        for harness in ("pi", "qwen-pi"):
            with self.subTest(harness=harness):
                self.app.register(harness, dict(self.target, harness=harness))
                self.app.harness_event(harness, {
                    "harness": harness, "type": "session", "session": harness,
                })
                self.app.auto = True
                self.app.harness_event(harness, {
                    "harness": harness, "type": "reply", "session": harness,
                    "turn": "one",
                    "text": "Full details.\n## TL;DR\nIt worked.",
                })
                self.app.playback.join(1)
                self.assertEqual(self.audio.spoken[-1], "It worked.")
                self.app.harness_event(harness, {
                    "harness": harness, "type": "reply", "session": harness,
                    "turn": "two", "text": "Missing summary.",
                })
                self.assertFalse(self.app.reply)
        self.assertEqual(self.audio.spoken, ["It worked.", "It worked."])

    def test_new_registration_clears_previous_reply_and_draft(self):
        self.app.stage("hello", "token-1")
        self.app.notify("token-1", self.event())
        self.app.register("token-2", self.target)
        self.assertIsNone(self.app.status()["reply"])
        self.assertFalse(self.app.status()["draft"])
        self.assertFalse(self.app.notify("token-1", self.event()))
        self.assertEqual(self.app.status()["phase"], "idle")

    def test_new_registration_resets_a_ready_tray_without_a_completion(self):
        self.app.stage("Existing draft", "token-1")
        self.app.register("token-2", self.target)
        self.assertEqual(self.app.status()["phase"], "idle")
        self.assertFalse(self.app.status()["draft"])

    def test_exited_session_never_receives_dictation_or_enter(self):
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

    def test_character_stays_fixed_and_only_samantha_switches_at_50_words(self):
        for playback in ("buffered", "streaming"):
            for character, words, expected in (
                ("samantha", 50, None),
                ("samantha", 51, "/newer.wav"),
                ("data", 1, "/data.wav"),
                ("data", 51, "/data.wav"),
            ):
                with self.subTest(
                    playback=playback, character=character, words=words
                ):
                    local = self.voice.LocalAudio(
                        Path(self.tmp.name),
                        {
                            "tts_url": "http://unused.test",
                            "playback_mode": playback,
                            "tts_voices": {
                                "data": {
                                    "label": "Data",
                                    "options": {
                                        "voice_ref": "/data.wav",
                                        "reference_text": "Data reference.",
                                    },
                                }
                            },
                            "tts_long_voice": {
                                "voice_ref": "/newer.wav",
                                "reference_text": "New reference.",
                            },
                        },
                    )
                    local.set_voice(character)
                    requests = []

                    def respond(request, *args, **kwargs):
                        requests.append(json.loads(request.data))
                        local.set_voice(
                            "data" if character == "samantha" else "samantha"
                        )
                        return io.BytesIO(speech_wav(b"\0\0" * 24))

                    with (
                        patch.object(
                            self.voice.urllib.request,
                            "urlopen",
                            side_effect=respond,
                        ),
                        patch.object(self.voice.subprocess, "Popen") as process,
                    ):
                        process.return_value.wait.return_value = 0
                        local.speak("word " * words, threading.Event())
                    self.assertTrue(requests)
                    for request in requests:
                        self.assertEqual(request.get("voice_ref"), expected)
                        self.assertEqual(
                            request.get("reference_text"),
                            {
                                None: None,
                                "/newer.wav": "New reference.",
                                "/data.wav": "Data reference.",
                            }[expected],
                        )
                    self.assertEqual(
                        sum(len(r["input"].split()) for r in requests), words
                    )

    def test_character_choice_persists_and_old_samantha_modes_migrate(self):
        preferences = Path(self.tmp.name) / "voice-mode"
        config = {
            "voice_preferences_path": str(preferences),
            "tts_voices": {
                "data": {
                    "label": "Data",
                    "options": {
                        "voice_ref": "/data.wav",
                        "reference_text": "Words.",
                    },
                }
            },
        }
        for old in ("auto", "current", "newer", "removed-character"):
            preferences.write_text(old)
            local = self.voice.LocalAudio(Path(self.tmp.name), config)
            self.assertEqual(local.selected_voice, "samantha")
        self.app.audio = local
        status = self.voice.dispatch(self.app, {"action": "voice:data"})
        self.assertEqual(status["selected_voice"], "data")
        restored = self.voice.LocalAudio(Path(self.tmp.name), config)
        self.assertEqual(restored.selected_voice, "data")
        with self.assertRaisesRegex(RuntimeError, "Unknown voice"):
            self.voice.dispatch(self.app, {"action": "voice:unknown"})
        self.assertEqual(preferences.read_text().strip(), "data")

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
