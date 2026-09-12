"""Offline ownership and host-selection regressions; never invokes real sudo."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HeadlessHomeSwitchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.bin = self.home / "bin"
        self.bin.mkdir()
        self.log = self.home / "commands"
        self.env = dict(
            os.environ, HOME=str(self.home),
            PATH=f"{self.bin}:{os.environ['PATH']}", CALL_LOG=str(self.log),
            TEST_HOST="relay", SERVICE_MODE="true", NIX_FAIL="0", NH_EXIT="0",
            HOME_NAMES=json.dumps(["william-darwin", "william@relay"]),
        )
        self.stub("uname", 'echo Darwin')
        self.stub("hostname", 'echo "$TEST_HOST"')
        self.stub("nix", '''
if [ "$NIX_FAIL" != 0 ]; then exit "$NIX_FAIL"; fi
case "$*" in
  *--apply*) printf '%s\\n' "$HOME_NAMES" ;;
  *) printf '%s\\n' "$SERVICE_MODE" ;;
esac''')
        self.stub("nh", (
            'printf "nh %s\\n" "$*" >> "$CALL_LOG"; exit "$NH_EXIT"'
        ))
        self.stub("sudo", 'printf "sudo %s\\n" "$*" >> "$CALL_LOG"')
        self.stub("launchctl", (
            'printf "launchctl %s\\n" "$*" >> "$CALL_LOG"; exit 1'
        ))
        flake = self.home / ".dotfiles/home/config/nix"
        flake.mkdir(parents=True)
        (flake / "nix.custom.conf").write_text("fixture")
        plists = self.home / ".local/share/dotfiles-system"
        plists.mkdir(parents=True)
        for name in ["limit.maxfiles", "io.tailscale.tailscaled"]:
            (plists / f"{name}.plist").write_text("fixture")

    def stub(self, name, body):
        path = self.bin / name
        path.write_text(f"#!{shutil.which('bash')}\nset -eu\n{body}\n")
        path.chmod(0o755)

    def run_switch(self):
        return subprocess.run(
            ["bash", str(ROOT / "home/config/bin/hmswitch"), "--verbose"],
            env=self.env, text=True, capture_output=True,
        )

    def calls(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_headless_home_switch_never_manages_system_services(self):
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--configuration william@relay --verbose", self.calls())
        self.assertNotIn("sudo", self.calls())
        self.assertNotIn("launchctl", self.calls())

    def test_registered_new_node_gets_its_own_home(self):
        self.env.update(
            TEST_HOST="new-mac",
            HOME_NAMES=json.dumps(["william-darwin", "william@new-mac"]),
        )
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--configuration william@new-mac", self.calls())

    def test_unregistered_node_preserves_platform_fallback(self):
        self.env["TEST_HOST"] = "other"
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--configuration william-darwin", self.calls())

    def test_evaluation_failure_makes_no_changes(self):
        self.env["NIX_FAIL"] = "23"
        result = self.run_switch()
        self.assertEqual(result.returncode, 23)
        self.assertEqual(self.calls(), "")

    def test_invalid_ownership_makes_no_changes(self):
        self.env["SERVICE_MODE"] = "invalid"
        result = self.run_switch()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls(), "")

    def test_failed_home_activation_does_not_trigger_system_work(self):
        self.env["NH_EXIT"] = "17"
        result = self.run_switch()
        self.assertEqual(result.returncode, 17)
        self.assertNotIn("sudo", self.calls())

    def test_legacy_mode_retains_existing_service_management(self):
        self.env["SERVICE_MODE"] = "false"
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ["limit.maxfiles", "io.tailscale.tailscaled"]:
            self.assertIn(
                "sudo launchctl bootstrap system "
                f"/Library/LaunchDaemons/{name}.plist", self.calls(),
            )


if __name__ == "__main__":
    unittest.main()
