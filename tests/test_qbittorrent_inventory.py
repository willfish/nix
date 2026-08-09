"""Unit tests for audiobook-library-import qBittorrent inventory helper."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).parents[1]
    / "home"
    / "config"
    / "llm"
    / "skills"
    / "audiobook-library-import"
    / "scripts"
    / "qbittorrent_inventory.py"
)


def load_script():
    import sys

    spec = importlib.util.spec_from_file_location(
        "qbittorrent_inventory",
        SCRIPT_PATH,
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # dataclasses looks up the module in sys.modules during class creation
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def bencode(value) -> bytes:
    """Minimal bencode encoder for fixtures (mirrors script decoder)."""
    if isinstance(value, int):
        return f"i{value}e".encode()
    if isinstance(value, bytes):
        return f"{len(value)}:".encode() + value
    if isinstance(value, str):
        raw = value.encode()
        return f"{len(raw)}:".encode() + raw
    if isinstance(value, list):
        return b"l" + b"".join(bencode(v) for v in value) + b"e"
    if isinstance(value, dict):
        items = []

        def key_bytes(x):
            return x if isinstance(x, bytes) else str(x).encode()

        for k in sorted(value.keys(), key=key_bytes):
            key = k if isinstance(k, bytes) else str(k).encode()
            items.append(bencode(key) + bencode(value[k]))
        return b"d" + b"".join(items) + b"e"
    raise TypeError(type(value))


class QbittorrentInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_script()

    def test_script_path_exists(self):
        self.assertTrue(
            SCRIPT_PATH.is_file(),
            f"missing shipped script {SCRIPT_PATH}",
        )

    def test_safe_join_rejects_traversal(self):
        mod = self.mod
        root = Path("/home/william/Downloads")
        self.assertIsNone(mod.safe_join_download(root, "../etc/passwd"))
        self.assertIsNone(mod.safe_join_download(root, "/etc/passwd"))
        joined = mod.safe_join_download(root, "Book Title.m4b")
        self.assertIsNotNone(joined)
        self.assertTrue(str(joined).endswith("Book Title.m4b"))

    def test_classify_status_complete_and_incomplete(self):
        mod = self.mod
        self.assertEqual(
            mod.classify_status(
                exists=True,
                finished_time=100,
                pieces=b"\xff",
                total_downloaded=1,
            ),
            "complete",
        )
        self.assertEqual(
            mod.classify_status(
                exists=False,
                finished_time=100,
                pieces=b"\xff",
                total_downloaded=1,
            ),
            "missing_path",
        )
        self.assertEqual(
            mod.classify_status(
                exists=True,
                finished_time=0,
                pieces=b"\x00",
                total_downloaded=0,
            ),
            "incomplete",
        )

    def test_inventory_from_fastresume_fixture(self):
        mod = self.mod
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            dl = tmp_path / "Downloads"
            fr_dir = tmp_path / "BT_backup"
            dl.mkdir()
            fr_dir.mkdir()

            complete_name = "Complete Book (Unabridged).m4b"
            incomplete_name = "Partial Course"
            missing_name = "Gone Already.m4b"

            (dl / complete_name).write_bytes(b"fake-audio-bytes")
            (dl / incomplete_name).mkdir()
            (dl / incomplete_name / "01.mp3").write_bytes(b"x")

            def write_fr(filename: str, payload: dict):
                (fr_dir / filename).write_bytes(bencode(payload))

            write_fr(
                "a.fastresume",
                {
                    b"name": complete_name.encode(),
                    b"qBt-savePath": str(dl).encode(),
                    b"finished_time": 12345,
                    b"paused": 0,
                    b"total_downloaded": 999,
                    b"pieces": b"\xff\xff",
                },
            )
            write_fr(
                "b.fastresume",
                {
                    b"name": incomplete_name.encode(),
                    b"qBt-savePath": str(dl).encode(),
                    b"finished_time": 0,
                    b"paused": 0,
                    b"total_downloaded": 0,
                    b"pieces": b"\x00\x00",
                },
            )
            write_fr(
                "c.fastresume",
                {
                    b"name": missing_name.encode(),
                    b"qBt-savePath": str(dl).encode(),
                    b"finished_time": 99,
                    b"paused": 0,
                    b"total_downloaded": 1,
                    b"pieces": b"\xff",
                },
            )

            records = mod.inventory(fr_dir, dl)
            self.assertEqual(len(records), 3)
            by_name = {r.name: r for r in records}
            self.assertEqual(by_name[complete_name].status, "complete")
            self.assertTrue(by_name[complete_name].exists)
            self.assertTrue(by_name[complete_name].is_audiobook_like)
            self.assertEqual(by_name[incomplete_name].status, "incomplete")
            self.assertEqual(by_name[missing_name].status, "missing_path")

            ready = mod.transfer_ready(records)
            self.assertEqual([r.name for r in ready], [complete_name])

            report = mod.format_markdown_report(records)
            self.assertIn("import candidate", report.lower())
            self.assertIn(complete_name, report)
            self.assertIn("Incomplete", report)

            nul = mod.emit_nul_list(ready)
            self.assertEqual(nul, complete_name.encode() + b"\x00")

    def test_main_markdown_exit_zero(self):
        mod = self.mod
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            dl = tmp_path / "Downloads"
            fr_dir = tmp_path / "BT_backup"
            out = tmp_path / "report.md"
            dl.mkdir()
            fr_dir.mkdir()
            name = "Sample Unabridged.m4b"
            (dl / name).write_bytes(b"data")
            (fr_dir / "x.fastresume").write_bytes(
                bencode(
                    {
                        b"name": name.encode(),
                        b"qBt-savePath": str(dl).encode(),
                        b"finished_time": 1,
                        b"paused": 0,
                        b"total_downloaded": 10,
                        b"pieces": b"\xff",
                    }
                )
            )
            rc = mod.main(
                [
                    "--fastresume-dir",
                    str(fr_dir),
                    "--download-root",
                    str(dl),
                    "--format",
                    "markdown",
                    "-o",
                    str(out),
                ]
            )
            self.assertEqual(rc, 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn(name, text)
            self.assertIn("complete", text)


if __name__ == "__main__":
    unittest.main()
