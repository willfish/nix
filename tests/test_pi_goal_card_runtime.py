"""Audit cards in real regular/fullscreen Pi, without credentials or models."""

import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PiGoalCardRuntimeTest(unittest.TestCase):
    def test_transcript_card_light_dark_narrow_wide_keyboard_and_mouse(self):
        binary = (
            os.environ.get("PI_GOAL_TEST_BIN")
            or os.environ.get("PI_SESSION_TEST_BIN")
            or shutil.which("pi")
        )
        self.assertTrue(binary, "Pi is required for offline card verification")
        files = os.environ.get("PI_HARNESS_TEST_HOME_FILES")
        source = (
            Path(files) / ".pi/agent/extensions/goal.ts"
            if files
            else ROOT / "home/config/pi/extensions/goal.ts"
        )
        for mode, theme, columns in [
            ("regular", "dark", 40),
            ("regular", "light", 110),
            ("fullscreen", "dark", 110),
            ("fullscreen", "light", 40),
        ]:
            with self.subTest(mode=mode, theme=theme, columns=columns):
                self.run_card(binary, source, mode, theme, columns)

    def run_card(self, binary, source, mode, theme, columns):
        with tempfile.TemporaryDirectory(prefix="pi-goal-card-") as directory:
            root = Path(directory)
            (root / "settings.json").write_text(
                json.dumps({"theme": theme, "enableInstallTelemetry": False})
            )
            master, slave = pty.openpty()
            fcntl.ioctl(
                slave,
                termios.TIOCSWINSZ,
                struct.pack("HHHH", 36, columns, 0, 0),
            )
            env = {
                **os.environ,
                "HOME": directory,
                "PI_CODING_AGENT_DIR": directory,
                "PI_GOAL_CARD_SOURCE": str(source),
                "CAPTURE_PROMPTS": "0",
                "PI_OFFLINE": "1",
                "PI_TELEMETRY": "0",
                "TERM": "xterm-256color",
                "COLORTERM": "truecolor",
            }
            child = subprocess.Popen(
                [
                    binary,
                    "--offline",
                    "--no-extensions",
                    "--no-skills",
                    "--no-context-files",
                    "--no-prompt-templates",
                    "--no-session",
                    "--tui-mode",
                    mode,
                    "--use-theme",
                    theme,
                    "-e",
                    str(ROOT / "tests/fixtures/pi-goal-card.ts"),
                ],
                stdin=slave,
                stdout=slave,
                stderr=slave,
                cwd=root,
                env=env,
                start_new_session=True,
            )
            os.close(slave)
            output = bytearray()

            def drain(seconds=0.1):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.02)[0]:
                        try:
                            data = os.read(master, 65536)
                        except OSError:
                            break
                        output.extend(data)
                        if b"\x1b]11;?" in data:
                            os.write(
                                master, b"\x1b]11;rgb:1a1a/1b1b/2626\x1b\\"
                            )
                        if b"\x1b[6n" in data:
                            os.write(master, b"\x1b[1;1R")

            def frame():
                try:
                    data = json.loads((root / "frame.json").read_text())
                    return re.sub(
                        r"\x1b\[[0-9;]*m", "", "\n".join(data["lines"])
                    )
                except (FileNotFoundError, json.JSONDecodeError):
                    return ""

            def until(predicate, label):
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    drain()
                    if predicate():
                        return
                    self.assertIsNone(
                        child.poll(), output.decode(errors="replace")
                    )
                self.fail(
                    f"{label}: frame={frame()!r}; "
                    f"output={output[-5000:].decode(errors='replace')!r}"
                )

            def command(text):
                os.write(master, text.encode() + b"\x1b[13u")

            try:
                drain(1)
                command("/goal Inspect source")
                drain(0.2)
                command("/goal verify")
                until(
                    lambda: "Audit running" in frame()
                    and "CURRENT_PATH.ts" in frame(),
                    "live card",
                )
                self.assertNotIn(
                    "HISTORY_ONLY", frame(), "activity starts collapsed"
                )
                until(
                    lambda: re.search(
                        r"Audit running \| [1-9][0-9]*s", frame()
                    ),
                    "elapsed time redraw",
                )
                os.write(master, b"\x0f")
                until(lambda: "HISTORY_ONLY" in frame(), "keyboard expansion")
                os.write(master, b"\x0f")
                until(
                    lambda: "HISTORY_ONLY" not in frame(), "keyboard collapse"
                )
                if mode == "fullscreen":
                    # Locate the card inside this disposable fixture without
                    # coupling to Pi's welcome/footer row count.
                    for row in range(1, 34):
                        os.write(
                            master, f"\x1b[<0;3;{row}M\x1b[<0;3;{row}m".encode()
                        )
                        drain(0.04)
                        if (root / "clicked.txt").exists():
                            break
                    self.assertTrue(
                        (root / "clicked.txt").exists(),
                        "card receives normalized mouse clicks",
                    )
                    until(lambda: "HISTORY_ONLY" in frame(), "mouse expansion")
                command("/fixture-finish")
                until(lambda: "Audit PASS" in frame(), "terminal card retained")
                if mode == "regular":
                    os.write(master, b"\x0f")
                until(
                    lambda: "EVIDENCE_DETAIL_ONLY" in frame(),
                    "retained expandable evidence",
                )
                self.assertNotIn(
                    "renderer failed", output.decode(errors="replace")
                )
                self.assertNotIn(
                    "exceeded terminal width", output.decode(errors="replace")
                )
                command("/goal status")
                until(
                    lambda: "Audit PASS" in frame(),
                    "card remains after user command",
                )
                capture = os.environ.get("PI_GOAL_CARD_FRAMES")
                if capture:
                    target = Path(capture)
                    target.mkdir(parents=True, exist_ok=True)
                    (target / f"{mode}-{theme}-{columns}.txt").write_text(
                        frame()
                    )
                command("/goal Another inspection")
                drain(0.2)
                command("/goal verify")
                until(lambda: "Audit running" in frame(), "second audit")
                command("/goal pause")
                until(
                    lambda: "Audit cancelled" in frame(),
                    "manual audit leaves editor available for pause",
                )
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                os.close(master)


if __name__ == "__main__":
    unittest.main()
