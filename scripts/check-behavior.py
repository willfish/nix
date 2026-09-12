#!/usr/bin/env python3
"""Run the complete offline behavioral gate against a built home generation."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
# This launcher is host-specific and references its deployed user profile.
# Its dedicated opt-in integration check is not a portable generation test.
ALLOWED_PYTHON_SKIPS = {
    "test_herdr_qwen_pi_runtime.QwenPiRuntimeTests."
    "test_launcher_reports_model_effort_changes_and_clears_on_exit",
}


def unexpected_node_skips(output: str) -> list[str]:
    return [
        line for line in output.splitlines()
        if re.search(r"#\s+SKIP(?:\s|$)", line, re.IGNORECASE)
        and not re.search(
            r"Set PI_(?:TEAM|VOICE)_LIVE_TEST=1 inside [Hh]erdr to opt in",
            line,
        )
    ]


def configure(home: Path) -> None:
    home = home.resolve(strict=True)
    if not str(home).startswith("/nix/store/"):
        raise ValueError("Select a built, immutable Home Manager generation")
    files = home / "home-files"
    binary = home / "home-path/bin/pi"
    required = [binary, files / ".pi/agent/extensions/mcp/index.ts",
                files / ".pi/agent/extensions/skill-catalog/index.ts"]
    for path in required:
        if not path.is_file():
            raise ValueError(f"Missing candidate runtime: {path}")
    os.environ.update({
        "MCP_TEST_HOME": str(home),
        "PI_MCP_TEST_BIN": str(binary), "PI_TEAM_TEST_BIN": str(binary),
        "PI_CONTEXT_TEST_BIN": str(binary), "PI_HERDR_TEST_BIN": str(binary),
        "PI_SESSION_TEST_BIN": str(binary),
        "PI_THEME_TEST_BIN": str(files / ".local/bin/pi"),
        "PI_SKILL_CATALOG_TEST_BIN": str(binary),
        "PI_MCP_TEST_EXTENSION": str(required[1]),
        "PI_SKILL_TEST_EXTENSION": str(required[2]),
        "PI_HARNESS_TEST_HOME_FILES": str(files),
        "PI_OFFLINE": "1", "PI_TELEMETRY": "0", "CAPTURE_PROMPTS": "0",
        "PI_TEAM_LIVE_TEST": "0", "PI_VOICE_LIVE_TEST": "0",
        "PYTHONDONTWRITEBYTECODE": "1",
    })
    # Never accidentally run a deployed, real-user Qwen launcher from CI.
    os.environ.pop("QWEN_PI_HERDR_TEST_BIN", None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generation", type=Path)
    args = parser.parse_args()
    configure(args.generation)
    os.chdir(ROOT)
    # Match `python -m unittest discover -s tests` including import semantics.
    sys.path.insert(0, str(ROOT / "tests"))
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tests"))
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    unexpected = [(case.id(), reason) for case, reason in result.skipped
                  if case.id() not in ALLOWED_PYTHON_SKIPS]
    if not result.wasSuccessful() or unexpected:
        print(f"Unexpected Python skips: {unexpected}", file=sys.stderr)
        return 1
    node = subprocess.run([
        "node", "--experimental-vm-modules", "--test", "--test-reporter=tap",
        *map(str, sorted((ROOT / "tests").glob("*.test.mjs"))),
    ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(node.stdout, end="", flush=True)
    skipped = unexpected_node_skips(node.stdout)
    if node.returncode or skipped:
        print(f"Unexpected Node skips: {skipped}", file=sys.stderr)
        return 1
    subprocess.run(
        ["bats", *map(str, sorted((ROOT / "tests").glob("*.bats")))],
        check=True)
    for name in ("native-mcp-runtime.py", "pi-mcp-runtime.py",
                 "pi-harness-slim-runtime.py"):
        subprocess.run([sys.executable, str(
            ROOT / "tests" / name), "-v"], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
