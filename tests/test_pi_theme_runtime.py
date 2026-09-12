"""Real Pi theme pairing, offline, in a private PTY with no user credentials."""

import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PiThemeRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        expression = f"""let
          f = builtins.getFlake {json.dumps(str(ROOT))};
          render = import {ROOT}/home/user/themes/render.nix {{
            lib = f.inputs.nixpkgs.lib;
          }};
          p = (import {ROOT}/home/user/themes/palettes.nix).foundation;
        in {{
          dark = render.pi "host-dark" p.dark;
          light = render.pi "host-light" p.light;
        }}"""
        cls.themes = json.loads(
            subprocess.check_output(
                ["nix", "eval", "--impure", "--json", "--expr", expression],
                text=True,
            )
        )
        cls.binary = os.environ.get("PI_THEME_TEST_BIN") or shutil.which("pi")
        if not cls.binary:
            raise RuntimeError(
                "Pi must be available for the offline theme regression"
            )

    def run_pair(self, columns, override=False):
        with tempfile.TemporaryDirectory(prefix="pi-theme-test-") as directory:
            root = Path(directory)
            for mode, theme in self.themes.items():
                (root / f"{mode}.json").write_text(json.dumps(theme))
            settings = {"theme": "dark", "enableInstallTelemetry": False}
            (root / "settings.json").write_text(json.dumps(settings))
            master, slave = pty.openpty()
            fcntl.ioctl(
                slave,
                termios.TIOCSWINSZ,
                struct.pack("HHHH", 30, columns, 0, 0),
            )
            args = [
                self.binary,
                "--offline",
                "--no-extensions",
                "--no-skills",
                "--no-context-files",
                "--no-prompt-templates",
                "--no-session",
                "--no-approve",
                "--no-themes",
                "--theme",
                str(root / "light.json"),
                "--theme",
                str(root / "dark.json"),
                "--use-theme",
                "host-light/host-dark",
            ]
            if override:
                args += ["--use-theme", "host-light"]
            env = {
                **os.environ,
                "HOME": directory,
                "PI_CODING_AGENT_DIR": directory,
                "CAPTURE_PROMPTS": "0",
                "TERM": "xterm-256color",
                "COLORTERM": "truecolor",
                "PI_OFFLINE": "1",
                "PI_TELEMETRY": "0",
            }
            child = subprocess.Popen(
                args,
                stdin=slave,
                stdout=slave,
                stderr=slave,
                cwd=directory,
                env=env,
                start_new_session=True,
            )
            os.close(slave)

            def foreground(mode):
                colour = self.themes[mode]["colors"]["muted"].lstrip("#")
                channels = ";".join(
                    str(int(colour[i : i + 2], 16)) for i in (0, 2, 4)
                )
                return f"\x1b[38;2;{channels}m".encode()

            def drain(seconds):
                output = b""
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if not select.select([master], [], [], 0.05)[0]:
                        continue
                    try:
                        data = os.read(master, 65536)
                    except OSError:
                        break
                    output += data
                    if b"\x1b]11;?" in data:
                        os.write(master, b"\x1b]11;rgb:1a1a/1b1b/2626\x1b\\")
                    if b"\x1b[?996n" in data:
                        os.write(master, b"\x1b[?997;1n")
                return output

            try:
                initial = drain(3)
                self.assertIsNone(
                    child.poll(), initial.decode(errors="replace")
                )
                self.assertIn(
                    foreground("light" if override else "dark"), initial
                )
                os.write(master, b"\x1b[?997;2n")
                light = drain(0.8)
                os.write(master, b"\x1b[?997;1n")
                dark = drain(0.8)
                if override:
                    self.assertNotIn(foreground("dark"), light + dark)
                else:
                    self.assertIn(foreground("light"), light)
                    self.assertIn(foreground("dark"), dark)
                self.assertEqual(
                    json.loads((root / "settings.json").read_text())["theme"],
                    "dark",
                )
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                os.close(master)

    def test_live_pair_at_narrow_and_wide_sizes(self):
        for columns in (50, 140):
            with self.subTest(columns=columns):
                self.run_pair(columns)

    def test_explicit_theme_overrides_pair_without_saving(self):
        self.run_pair(90, override=True)


if __name__ == "__main__":
    unittest.main()
