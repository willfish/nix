"""Exercise capture lifecycle with real producers, without opening a mic."""

import array
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import wave


MODULE = (
    Path(__file__).resolve().parents[1] / "home/config/voice/voice_capture.py"
)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Capture adapter is not implemented")
        spec = importlib.util.spec_from_file_location("voice_capture", MODULE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "capture.wav"

    def capture(self, script):
        capture = self.module.PipeWireCapture(
            self.path, command=[sys.executable, "-u", "-c", script]
        )
        self.addCleanup(capture.close)
        return capture

    def test_preferred_microphone_is_passed_as_a_stable_pipewire_target(self):
        original = self.module.subprocess.Popen
        commands = []

        def producer(args, **kwargs):
            commands.append(args)
            return original([
                sys.executable, "-c", "import os; os.write(1,b'\\0\\0'*160)"
            ], **kwargs)

        with patch.object(self.module.subprocess, "Popen", producer):
            capture = self.module.PipeWireCapture(
                self.path, target="alsa_input.usb-microphone"
            )
            self.addCleanup(capture.close)
            self.assertTrue(capture.wait_ready(1))
            self.assertEqual(capture.wait(1), 0)
        target_at = commands[0].index("--target")
        self.assertEqual(commands[0][target_at + 1],
                         "alsa_input.usb-microphone")

    def test_chunker_splits_on_pause_and_at_the_force_limit(self):
        chunker = self.module.SpeechChunker(
            silence_frames=5, force_frames=20, lookback_frames=10
        )
        voiced = array.array("h", [4000] * (8 * 320))
        self.assertEqual(chunker.push(voiced), [])
        silence = array.array("h", [0] * (5 * 320))
        split = chunker.push(silence)
        self.assertEqual(len(split), 1)
        self.assertEqual(len(split[0]), 8 * 320)
        self.assertIsNone(chunker.flush())

        forced = self.module.SpeechChunker(
            silence_frames=50, force_frames=10, lookback_frames=4
        )
        long_voiced = array.array("h", [4000] * (12 * 320))
        out = forced.push(long_voiced)
        self.assertEqual(len(out), 1)
        self.assertEqual(len(out[0]), 10 * 320)
        remainder = forced.flush()
        self.assertEqual(len(remainder), 2 * 320)

    def test_pause_emits_a_chunk_before_capture_ends(self):
        capture = self.capture(
            "import os, signal, sys, time\n"
            "signal.signal(signal.SIGINT, lambda *_: sys.exit(0))\n"
            "frame = b'\\x00\\x20' * 320\n"
            "quiet = b'\\x00\\x00' * 320\n"
            "os.write(1, frame * 10)\n"
            "time.sleep(0.05)\n"
            "os.write(1, quiet * 40)\n"
            "time.sleep(0.05)\n"
            "os.write(1, frame * 10)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(2))
        chunks = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            chunks.extend(capture.drain_chunks())
            if chunks:
                break
            capture.wait_progress(0.05)
        self.assertTrue(chunks)
        self.assertIsNone(capture.poll())
        self.assertEqual(len(chunks[0]), 10 * 320)
        capture.close()
        self.assertEqual(capture.wait(1), 0)
        rest = capture.drain_chunks()
        self.assertTrue(rest)
        self.assertEqual(len(rest[0]), 10 * 320)

    def test_clipping_is_visible_for_saturated_audio(self):
        capture = self.capture(
            "import os; os.write(1, b'\\xff\\x7f' * 320)"
        )
        self.assertTrue(capture.wait_ready(1))
        self.assertEqual(capture.wait(1), 0)
        self.assertTrue(capture.clipping)

    def test_ready_means_pcm_arrived_and_wait_finalizes_wav(self):
        capture = self.capture(
            "import os, signal, sys, time\n"
            "signal.signal(signal.SIGINT, lambda *_: sys.exit(0))\n"
            "os.write(1, b'\\x00\\x20' * 320)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(2))
        self.assertGreater(capture.started_at, 0)
        self.assertAlmostEqual(capture.level, 0.25, places=3)
        self.assertIsNone(capture.poll())
        capture.send_signal(signal.SIGINT)
        self.assertEqual(capture.wait(2), 0)
        self.assertEqual(capture.poll(), 0)
        self.assertIsNone(capture.error)
        with wave.open(str(self.path), "rb") as wav:
            self.assertEqual(wav.getframerate(), 16000)
            self.assertEqual(wav.getnchannels(), 1)
            self.assertEqual(wav.getsampwidth(), 2)
            self.assertEqual(wav.getnframes(), 320)
            self.assertEqual(wav.readframes(320), b"\x00\x20" * 320)

    def test_no_samples_is_a_failed_start(self):
        capture = self.capture("pass")
        with self.assertRaisesRegex(RuntimeError, "samples"):
            capture.wait_ready(1)
        self.assertEqual(capture.wait(1), 0)
        self.assertIsNone(capture.started_at)

    def test_producer_without_samples_times_out_and_is_reaped(self):
        capture = self.capture("import time; time.sleep(10)")
        with self.assertRaisesRegex(RuntimeError, "samples"):
            capture.wait_ready(0.1)
        self.assertIsNotNone(capture.poll())
        self.assertIsNone(capture.started_at)

    def test_start_waits_for_delayed_samples(self):
        capture = self.capture(
            "import os, time\n"
            "time.sleep(0.2)\n"
            "os.write(1, b'\\0\\0' * 160)\n"
        )
        started = time.monotonic()
        self.assertTrue(capture.wait_ready(2))
        self.assertGreaterEqual(capture.started_at - started, 0.15)
        self.assertEqual(capture.wait(1), 0)

    def test_pcm_sample_boundaries_are_preserved_across_reads(self):
        capture = self.capture(
            "import os, time\n"
            "os.write(1, b'\\x00')\n"
            "time.sleep(0.1)\n"
            "os.write(1, b'\\x20' + b'\\x00\\x20' * 99)\n"
        )
        started = time.monotonic()
        self.assertTrue(capture.wait_ready(1))
        self.assertGreaterEqual(capture.started_at - started, 0.08)
        self.assertEqual(capture.wait(1), 0)
        self.assertAlmostEqual(capture.level, 0.25, places=3)
        with wave.open(str(self.path), "rb") as wav:
            self.assertEqual(wav.readframes(100), b"\x00\x20" * 100)

    def test_abnormal_exit_keeps_code_and_reports_failure(self):
        capture = self.capture(
            "import os, time\n"
            "os.write(1, b'\\0\\0' * 160)\n"
            "time.sleep(0.1)\n"
            "raise SystemExit(7)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        self.assertEqual(capture.wait(1), 7)
        self.assertRegex(capture.error or "", "exited.*7")

    def test_pipewire_exit_one_after_requested_stop_is_success(self):
        capture = self.capture(
            "import os, signal, sys, time\n"
            "signal.signal(signal.SIGINT, lambda *_: sys.exit(1))\n"
            "os.write(1, b'\\0\\0' * 320)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        capture.close()
        self.assertEqual(capture.wait(1), 0)
        self.assertEqual(capture.poll(), 0)
        self.assertIsNone(capture.error)
        with wave.open(str(self.path), "rb") as wav:
            self.assertEqual(wav.getnframes(), 320)

    def test_exit_one_without_requested_stop_remains_a_failure(self):
        capture = self.capture(
            "import os\n"
            "os.write(1, b'\\0\\0' * 320)\n"
            "raise SystemExit(1)\n"
        )
        self.assertEqual(capture.wait(1), 1)
        self.assertRegex(capture.error or "", "exited.*1")

    def test_stopping_does_not_hide_reported_pipewire_errors(self):
        capture = self.capture(
            "import os, signal, sys, time\n"
            "signal.signal(signal.SIGINT, lambda *_: sys.exit(1))\n"
            "os.write(2, b'error: connection lost\\n')\n"
            "os.write(1, b'\\0\\0' * 320)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        capture.close()
        self.assertEqual(capture.poll(), 1)
        self.assertRegex(capture.error or "", "exited.*1")

    def test_pipewire_sample_limit_exit_one_is_success(self):
        with patch.object(self.module, "SAMPLE_LIMIT", 320, create=True):
            capture = self.capture(
                "import os\n"
                "os.write(1, b'\\0\\0' * 320)\n"
                "raise SystemExit(1)\n"
            )
            self.assertEqual(capture.wait(1), 0)
            self.assertIsNone(capture.error)

    def test_stream_that_stalls_after_readiness_is_killed(self):
        capture = self.capture(
            "import os, time\n"
            "os.write(1, b'\\0\\0' * 160)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        try:
            result = capture.wait(4)
        except TimeoutError:
            self.fail("Stalled microphone was not stopped")
        except self.module.subprocess.TimeoutExpired:
            self.fail("Stalled microphone was not stopped")
        self.assertNotEqual(result, 0)
        self.assertRegex(capture.error or "", "stalled")

    def test_closed_stream_with_live_producer_reports_disconnect(self):
        capture = self.capture(
            "import os, time\n"
            "os.write(1, b'\\0\\0' * 160)\n"
            "os.close(1)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        try:
            result = capture.wait(1)
        except self.module.subprocess.TimeoutExpired:
            self.fail("Disconnected producer was not reaped")
        self.assertNotEqual(result, 0)
        self.assertRegex(capture.error or "", "disconnected")

    def test_close_escalates_ignored_signals_and_is_idempotent(self):
        capture = self.capture(
            "import os, signal, time\n"
            "signal.signal(signal.SIGINT, signal.SIG_IGN)\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "os.write(1, b'\\0\\0' * 160)\n"
            "time.sleep(10)\n"
        )
        self.assertTrue(capture.wait_ready(1))
        started = time.monotonic()
        try:
            capture.close()
        except self.module.subprocess.TimeoutExpired:
            self.fail("Close did not escalate an unresponsive producer")
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(capture.poll(), -signal.SIGKILL)
        capture.close()
        capture.send_signal(signal.SIGINT)

    def test_constructor_failure_removes_partial_wav(self):
        with self.assertRaises(FileNotFoundError):
            self.module.PipeWireCapture(
                self.path, command=[str(Path(self.tmp.name) / "absent")]
            )
        self.assertFalse(self.path.exists())

    def test_reader_start_failure_cleans_up_already_spawned_process(self):
        processes = []
        popen = self.module.subprocess.Popen

        def spawn(*args, **kwargs):
            process = popen(*args, **kwargs)
            processes.append(process)
            return process

        with patch.object(self.module.subprocess, "Popen", side_effect=spawn):
            with patch.object(
                self.module.threading.Thread, "start",
                side_effect=RuntimeError("Threads exhausted"),
            ):
                with self.assertRaisesRegex(RuntimeError, "exhausted"):
                    self.capture("import time; time.sleep(10)")
        try:
            self.assertFalse(self.path.exists())
            self.assertIsNotNone(processes[0].poll())
        finally:
            processes[0].kill()
            processes[0].wait(1)
            processes[0].stdout.close()

    def test_cancellation_before_readiness_reaps_the_producer(self):
        capture = self.capture("import time; time.sleep(10)")
        capture.close()
        self.assertIsNotNone(capture.poll())
        with self.assertRaisesRegex(RuntimeError, "samples"):
            capture.wait_ready(0.1)

    @unittest.skipUnless(Path("/dev/full").exists(), "requires /dev/full")
    def test_wav_write_failure_finishes_reader_and_reaps_producer(self):
        self.path.symlink_to("/dev/full")
        capture = self.capture(
            "import os, time\n"
            "os.write(1, b'\\0\\0' * 8192)\n"
            "time.sleep(10)\n"
        )
        try:
            capture.wait(1)
        except self.module.subprocess.TimeoutExpired:
            self.fail("WAV write failure left capture unfinished")
        self.assertIsNotNone(capture.poll())
        self.assertRegex(capture.error or "", "WAV|capture failed")


if __name__ == "__main__":
    unittest.main()
