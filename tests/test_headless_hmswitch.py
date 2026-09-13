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
        self.system = self.home / "system"
        self.generation = self.home / "home-generation"
        (self.system / "etc/dotfiles").mkdir(parents=True)
        self.generation.mkdir()
        (self.generation / "activate").write_text("#!/bin/sh\nexit 0\n")
        (self.generation / "activate").chmod(0o755)
        (self.system / "etc/dotfiles/home-generation").write_text(
            str(self.generation) + "\n"
        )
        self.env = dict(
            os.environ, HOME=str(self.home),
            PATH=f"{self.bin}:{os.environ['PATH']}", CALL_LOG=str(self.log),
            TEST_HOST="relay", SERVICE_MODE="true", NIX_FAIL="0", NH_EXIT="0",
            BUILD_EXIT="0", SUDO_EXIT="0", SYSTEM_TARGET=str(self.system),
            HOME_TARGET=str(self.generation),
            HOME_NAMES=json.dumps(["william-darwin", "william@relay"]),
        )
        self.stub("uname", 'echo Darwin')
        self.stub("hostname", 'echo "$TEST_HOST"')
        self.stub("nix", '''
if [ "$NIX_FAIL" != 0 ]; then exit "$NIX_FAIL"; fi
case "$*" in
  build*)
    printf 'nix %s\\n' "$*" >> "$CALL_LOG"
    if [ "$BUILD_EXIT" != 0 ]; then exit "$BUILD_EXIT"; fi
    printf '%s\\n' "$SYSTEM_TARGET" ;;
  *--apply*) printf '%s\\n' "$HOME_NAMES" ;;
  *) printf '%s\\n' "$SERVICE_MODE" ;;
esac''')
        self.stub("nh", (
            'printf "nh %s\\n" "$*" >> "$CALL_LOG"; exit "$NH_EXIT"'
        ))
        self.stub("sudo", (
            'printf "sudo %s\\n" "$*" >> "$CALL_LOG"; exit "$SUDO_EXIT"'
        ))
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

    def run_switch(self, *args, input_text=""):
        return subprocess.run(
            ["bash", str(ROOT / "home/config/bin/hmswitch"), *args],
            env=self.env, text=True, capture_output=True, input=input_text,
        )

    def calls(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_plain_switch_deploys_system_before_its_exact_home(self):
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls().splitlines()
        self.assertEqual(len(calls), 3)
        self.assertIn('darwinConfigurations."relay".system', calls[0])
        self.assertIn("--no-write-lock-file", calls[0])
        self.assertEqual(
            calls[1], f"sudo {self.system}/sw/bin/darwin-deploy {self.system}"
        )
        self.assertEqual(calls[2], f"nh home switch {self.generation}")
        self.assertNotIn("launchctl", self.calls())

    def test_registered_new_node_gets_its_own_system(self):
        self.env.update(
            TEST_HOST="new-mac",
            HOME_NAMES=json.dumps(["william-darwin", "william@new-mac"]),
        )
        result = self.run_switch("--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('darwinConfigurations."new-mac".system', self.calls())
        self.assertIn(
            f"nh home switch {self.generation} --verbose", self.calls()
        )

    def test_already_active_pair_needs_no_sudo(self):
        setup = self.home / "existing-system.sh"
        setup.write_text('''cd() {
  case "$1" in
    /run/current-system|/nix/var/nix/profiles/system)
      builtin cd "$SYSTEM_TARGET" ;;
    *) builtin cd "$@" ;;
  esac
}
''')
        self.env["BASH_ENV"] = str(setup)
        self.stub("cat", 'printf "%s\\n" "$HOME_TARGET"')
        result = self.run_switch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("sudo", self.calls())
        self.assertIn(f"nh home switch {self.generation}", self.calls())

    def test_dry_run_never_deploys(self):
        for flag in ["--dry", "-n"]:
            with self.subTest(flag=flag):
                result = self.run_switch(flag)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("sudo", self.calls())
                self.assertIn(
                    f"nh home switch {self.generation} {flag}", self.calls()
                )

    def test_declined_confirmation_does_not_activate_either_generation(self):
        result = self.run_switch("--ask", input_text="n\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("sudo", self.calls())
        self.assertNotIn("nh home switch", self.calls())

    def test_accepted_confirmation_is_also_forwarded_to_home(self):
        result = self.run_switch("--ask", input_text="y\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("sudo", self.calls())
        self.assertIn(f"nh home switch {self.generation} --ask", self.calls())

    def test_unsupported_arguments_fail_before_build_or_activation(self):
        for arg in [
            "--update", "--configuration=other", "--unknown", "other-flake"
        ]:
            with self.subTest(arg=arg):
                result = self.run_switch(arg)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.calls(), "")

    def test_build_failure_makes_no_activation_changes(self):
        self.env["BUILD_EXIT"] = "19"
        result = self.run_switch()
        self.assertEqual(result.returncode, 19)
        self.assertNotIn("sudo", self.calls())
        self.assertNotIn("nh home switch", self.calls())

    def test_system_failure_stops_before_home(self):
        self.env["SUDO_EXIT"] = "21"
        result = self.run_switch()
        self.assertEqual(result.returncode, 21)
        self.assertNotIn("nh home switch", self.calls())

    def test_missing_pinned_home_stops_before_sudo(self):
        (self.generation / "activate").unlink()
        result = self.run_switch()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("sudo", self.calls())
        self.assertNotIn("nh home switch", self.calls())

    def test_unregistered_legacy_node_preserves_platform_fallback(self):
        self.env.update(TEST_HOST="other", SERVICE_MODE="false")
        result = self.run_switch("--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--configuration william-darwin --verbose", self.calls())
        self.assertNotIn("darwin-deploy", self.calls())

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

    def test_failed_home_activation_does_not_retry_system_work(self):
        self.env["NH_EXIT"] = "17"
        result = self.run_switch()
        self.assertEqual(result.returncode, 17)
        self.assertEqual(self.calls().count("sudo "), 1)
        self.assertEqual(self.calls().count("nh home switch"), 1)

    def test_linux_remains_home_only(self):
        self.stub("uname", "echo Linux")
        self.env["TEST_HOST"] = "foundation"
        result = self.run_switch("--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "--configuration william@foundation --verbose", self.calls()
        )
        self.assertNotIn("sudo", self.calls())
        self.assertNotIn("nix build", self.calls())

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
