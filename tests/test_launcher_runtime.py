"""Launcher fallback contract without a display or resident services."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/launcher/launch.sh"


class LauncherRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.log = self.directory / "calls"
        self.env = {**os.environ, "CALLS": str(self.log)}
        self.env["PATH"] = str(self.directory) + ":" + os.environ["PATH"]
        for name in (
            "systemctl",
            "elephant",
            "walker",
            "hypr-theme-seed",
            "hypr-launcher-fallback",
        ):
            self.command(name)

    def command(self, name, status=0):
        script = self.directory / name
        script.write_text(
            f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "$CALLS"\n'
            f'exit {status}\n'
        )
        script.chmod(0o755)

    def run_launcher(self, *args):
        result = subprocess.run(
            ["bash", "-euo", "pipefail", str(SCRIPT), *args],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.log.read_text().splitlines()

    def test_healthy_backend_opens_walker(self):
        calls = self.run_launcher()
        self.assertEqual(
            calls,
            [
                "hypr-theme-seed ",
                "systemctl --user start elephant.service walker.service",
                "elephant query desktopapplications;;1",
                "walker ",
            ],
        )

    def test_explicit_fallback_does_not_start_services(self):
        self.assertEqual(
            self.run_launcher("--fallback", "--dmenu"),
            ["hypr-launcher-fallback --dmenu"],
        )

    def test_start_failure_falls_back(self):
        self.command("systemctl", 1)
        calls = self.run_launcher()
        self.assertEqual(calls[-1], "hypr-launcher-fallback ")
        self.assertFalse(any(c.startswith("walker ") for c in calls))

    def test_unresponsive_backend_falls_back(self):
        self.command("elephant", 1)
        calls = self.run_launcher()
        self.assertEqual(calls[-1], "hypr-launcher-fallback ")
        self.assertFalse(any(c.startswith("walker ") for c in calls))

    def test_frontend_failure_falls_back(self):
        self.command("walker", 1)
        self.assertEqual(self.run_launcher()[-1], "hypr-launcher-fallback ")

    def test_backend_timeout_is_bounded(self):
        script = self.directory / "elephant"
        script.write_text("#!/bin/sh\nsleep 10\n")
        self.assertEqual(self.run_launcher()[-1], "hypr-launcher-fallback ")

    def test_user_query_is_not_interpreted_by_wrapper(self):
        marker = self.directory / "unsafe"
        query = f"$(touch {marker})"
        calls = self.run_launcher("--query", query)
        self.assertEqual(calls[-1], f"walker --query {query}")
        self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
