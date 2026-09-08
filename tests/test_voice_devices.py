"""Microphone routing probes never open audio streams or mutate defaults."""

import importlib.util
import json
from pathlib import Path
import subprocess
import threading
import time
import unittest


MODULE = (
    Path(__file__).resolve().parents[1] / "home/config/voice/voice_devices.py"
)


def node(name, label, muted=False):
    return {
        "type": "PipeWire:Interface:Node", "info": {
            "props": {"node.name": name, "node.description": label,
                      "media.class": "Audio/Source"},
            "params": {"Props": [{"mute": muted}]},
        },
    }


def devices(default="webcam"):
    return [
        node("webcam", "Webcam"), node("headset", "Headset", True),
        {"props": {"metadata.name": "default"}, "metadata": [
            {"key": "default.audio.source", "value": {"name": default}},
        ]},
    ]


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Device monitor is not implemented")
        spec = importlib.util.spec_from_file_location("voice_devices", MODULE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.rows = devices()
        self.calls = []

    def probe_run(self, args, **kwargs):
        self.calls.append((args, kwargs))
        return subprocess.CompletedProcess(args, 0, json.dumps(self.rows), "")

    def monitor(self, preferred=None):
        return self.module.MicrophoneMonitor(preferred, runner=self.probe_run)

    def test_default_routing_is_left_to_pipewire(self):
        result = self.monitor().resolve()
        self.assertEqual(result["name"], "Webcam")
        self.assertIsNone(result["target"])
        self.assertFalse(result["missing"])
        self.assertFalse(result["muted"])
        self.assertEqual(self.calls[0][0], ["pw-dump"])
        self.assertLessEqual(self.calls[0][1]["timeout"], 2)

    def test_preferred_source_uses_stable_name_and_reports_mute(self):
        result = self.monitor("headset").resolve()
        self.assertEqual(result["name"], "Headset")
        self.assertEqual(result["target"], "headset")
        self.assertTrue(result["muted"])
        self.assertFalse(result["missing"])

    def test_unplugged_preference_falls_back_to_current_default(self):
        monitor = self.monitor("headset")
        self.assertEqual(monitor.resolve()["target"], "headset")
        self.rows = [row for row in self.rows if row != self.rows[1]]
        result = monitor.resolve()
        self.assertIsNone(result["target"])
        self.assertEqual(result["name"], "Webcam")
        self.assertTrue(result["missing"])

    def test_probe_failure_never_reuses_a_stale_device_target(self):
        monitor = self.monitor("headset")
        self.assertEqual(monitor.resolve()["target"], "headset")

        def failed(*args, **kwargs):
            raise subprocess.TimeoutExpired("pw-dump", 2)

        monitor.runner = failed
        result = monitor.resolve()
        self.assertIsNone(result["target"])
        self.assertTrue(result["missing"])
        self.assertIn("unavailable", result["error"].lower())

    def test_status_never_waits_for_probe_or_creates_duplicate_workers(self):
        began, release = threading.Event(), threading.Event()

        def slow(args, **kwargs):
            began.set()
            release.wait(1)
            return self.probe_run(args, **kwargs)

        monitor = self.module.MicrophoneMonitor(runner=slow)
        try:
            started = time.monotonic()
            result = monitor.status()
            self.assertLess(time.monotonic() - started, 0.1)
            self.assertTrue(began.wait(0.5))
            for _ in range(20):
                self.assertEqual(monitor.status(), result)
            release.set()
            deadline = time.monotonic() + 1
            while monitor.status()["name"] != "Webcam":
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.assertEqual(len(self.calls), 1)
        finally:
            release.set()

    def test_resolve_refreshes_device_state_and_updates_cached_status(self):
        monitor = self.monitor()
        self.assertEqual(monitor.resolve()["name"], "Webcam")
        self.rows = devices(default="headset")
        self.assertEqual(monitor.status()["name"], "Webcam")
        self.assertEqual(monitor.resolve()["name"], "Headset")
        self.assertEqual(monitor.status()["name"], "Headset")

    def test_missing_default_does_not_choose_an_arbitrary_microphone(self):
        self.rows = devices(default="disconnected")
        result = self.monitor().resolve()
        self.assertIsNone(result["target"])
        self.assertEqual(result["name"], "System default")
        self.assertEqual(result["error"], "No default microphone available")

    def test_string_metadata_is_supported_and_sink_monitors_are_excluded(self):
        self.rows[-1]["metadata"][0]["value"] = '{"name":"webcam"}'
        speaker = node("speakers", "Desktop audio")
        speaker["info"]["props"]["media.class"] = "Audio/Sink"
        self.rows.append(speaker)
        result = self.monitor("speakers").resolve()
        self.assertEqual(result["name"], "Webcam")
        self.assertTrue(result["missing"])
        self.assertIsNone(result["target"])


if __name__ == "__main__":
    unittest.main()
