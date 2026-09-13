"""Offline tests for the youtube-extract helper."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (
    ROOT
    / "home/config/llm/skills/youtube-extract/scripts/youtube_extract.py"
)


def load_script():
    import sys

    spec = importlib.util.spec_from_file_location("youtube_extract", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


VTT = """WEBVTT
Kind: captions
Language: en

00:03:34.080 --> 00:03:40.309
at what I'm talking about. This is the
Quen 3.8 27 billion GSQRCO GGUF. It's a

00:03:40.319 --> 00:03:43.350
Quen 3.8 27 billion GSQRCO GGUF. It's a
nonuniform GGUF quantization produced

00:17:31.000 --> 00:17:34.000
that I have the settings here and I got
the prompt library.

00:17:34.000 --> 00:17:34.000
that I have the settings here and I got
the prompt library.
"""

SRT = """1
00:00:01,000 --> 00:00:02,500
Hello world

2
00:00:03,000 --> 00:00:04,000
Second line
"""


class YoutubeExtractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_script()

    def test_script_path_exists(self):
        self.assertTrue(SCRIPT.is_file())

    def test_parse_url_and_timestamp(self):
        ref = self.mod.parse_video_ref(
            "https://www.youtube.com/watch?v=qILTuXLxfBM&t=1051s"
        )
        self.assertEqual(ref.video_id, "qILTuXLxfBM")
        self.assertEqual(ref.timestamp, 1051.0)
        short = self.mod.parse_video_ref(
            "https://youtu.be/qILTuXLxfBM?t=17m31s"
        )
        self.assertEqual(short.video_id, "qILTuXLxfBM")
        self.assertEqual(short.timestamp, 1051.0)

    def test_parse_offset_formats(self):
        self.assertEqual(self.mod.parse_offset("1051"), 1051.0)
        self.assertEqual(self.mod.parse_offset("1h2m3s"), 3723.0)
        self.assertIsNone(self.mod.parse_offset(None))

    def test_vtt_dedupes_rolling_captions(self):
        cues = self.mod.dedupe_rolling(self.mod.parse_cues(VTT))
        texts = [cue.text for cue in cues]
        self.assertEqual(len(cues), 3)
        self.assertTrue(texts[0].startswith("at what I'm talking about"))
        self.assertIn("nonuniform GGUF", texts[1])
        self.assertEqual(texts.count(texts[2]), 1)

    def test_srt_and_slice_and_search(self):
        cues = self.mod.parse_cues(SRT)
        self.assertEqual(len(cues), 2)
        self.assertEqual(cues[0].text, "Hello world")
        window = self.mod.slice_cues(cues, 1.5, 1.0)
        self.assertEqual([cue.text for cue in window], ["Hello world"])
        hits = self.mod.search_cues(cues, "SECOND")
        self.assertEqual([cue.text for cue in hits], ["Second line"])

    def test_cli_uses_fixtures_not_network(self):
        with tempfile.TemporaryDirectory() as raw:
            work = Path(raw)
            subs = work / "captions.vtt"
            info = work / "info.json"
            subs.write_text(VTT, encoding="utf-8")
            info.write_text(json.dumps({
                "id": "qILTuXLxfBM",
                "title": "16GB Is All You Need for Serious AI",
                "channel": "Example",
                "duration": 1610,
                "duration_string": "26:50",
                "webpage_url": "https://www.youtube.com/watch?v=qILTuXLxfBM",
                "description": "Qwen3.8 27B GSQ plus RCO",
                "chapters": [{"start_time": 214, "title": "The model"}],
            }), encoding="utf-8")
            payload = json.loads(subprocess.check_output(
                [
                    "python3",
                    str(SCRIPT),
                    "--info-file",
                    str(info),
                    "--subs-file",
                    str(subs),
                    "--around",
                    "214",
                    "--query",
                    "GGUF",
                    "--json",
                ],
                text=True,
            ))
        self.assertEqual(
            payload["title"],
            "16GB Is All You Need for Serious AI",
        )
        self.assertEqual(payload["chapters"][0]["title"], "The model")
        self.assertGreaterEqual(payload["cue_count"], 2)
        self.assertTrue(any("Quen" in cue["text"] for cue in payload["around"]))
        self.assertTrue(payload["matches"])

    def test_yt_dlp_command_never_downloads(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("--skip-download", source)
        self.assertNotRegex(source, r"(?m)^\s*-x\b")
        self.assertNotIn("--extract-audio", source)


if __name__ == "__main__":
    unittest.main()
