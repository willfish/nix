"""Installer selection with synthetic credentials and Nix processes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PublicInstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.home = self.root / "home with spaces"
        self.home.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls"
        self.generation = self.root / "generation"
        self.generation.mkdir()
        self.executable(
            self.generation / "activate", 'echo activate >> "$CALL_LOG"')
        self.probe = self.root / "probe/bin"
        self.probe.mkdir(parents=True)
        self.executable(
            self.probe / "probe-private-access", 'exit "$PROBE_EXIT"')
        dbus = self.root / "dbus/bin"
        dbus.mkdir(parents=True)
        self.executable(dbus / "dbus-run-session", (
            'echo dbus >> "$CALL_LOG"; '
            'while [ "$1" != -- ]; do shift; done; shift; exec "$@"'
        ))
        self.env = dict(
            os.environ, DBUS_SESSION_BUS_ADDRESS="synthetic",
            DBUS_ROOT=str(dbus.parent), HOME=str(self.home),
            PATH=f"{self.bin}:{os.environ['PATH']}",
            XDG_STATE_HOME=str(self.home / "state"), CALL_LOG=str(self.log),
            HMSWITCH_FLAKE=str(ROOT), FETCH_EXIT="0", PROBE_EXIT="1",
            BUILD_EXIT="0", BUS_EXIT="0", GENERATION=str(self.generation),
            PROBE_ROOT=str(self.probe.parent),
        )
        self.executable(self.bin / "id", (
            'if [ "$1" = -un ]; then echo visitor; else echo 1000; fi'
        ))
        self.executable(self.bin / "nix", '''
printf '%s private=%s user=%s home=%s\n' \\
  "$*" "$DOTFILES_PRIVATE" "$DOTFILES_USER" "$DOTFILES_HOME" >> "$CALL_LOG"
case "$*" in
  *"inputs.agent-bus"*) exit "$BUS_EXIT" ;;
  *" eval "*) echo /synthetic-private-source; exit "$FETCH_EXIT" ;;
  *"#private-access-probe"*) echo "$PROBE_ROOT" ;;
  *"#activation-dbus"*) echo "$DBUS_ROOT" ;;
  *)
    test "$BUILD_EXIT" = 0 || exit "$BUILD_EXIT"
    while [ "$#" -gt 0 ]; do
      if [ "$1" = --out-link ]; then ln -s "$GENERATION" "$2"; break; fi
      shift
    done ;;
esac
''')

    def executable(self, path, body):
        path.write_text(f"#!{shutil.which('bash')}\nset -eu\n{body}\n")
        path.chmod(0o755)

    def key(self):
        (self.home / ".ssh").mkdir()
        (self.home / ".ssh/id_ed25519").write_text("not a real key")

    def run_install(self, *args):
        return subprocess.run(
            ["bash", str(ROOT / "scripts/install-home"), *args],
            env=self.env, text=True, capture_output=True,
        )

    def calls(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_no_key_does_not_evaluate_private_source(self):
        result = self.run_install("--check-private")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "false\n")
        self.assertEqual(self.calls(), "")

    def test_missing_private_repository_fails_closed(self):
        self.key()
        self.env["FETCH_EXIT"] = "1"
        result = self.run_install("--check-private")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "false\n")
        self.assertNotIn("#private-access-probe", self.calls())

    def test_existing_key_is_not_enough(self):
        self.key()
        result = self.run_install("--check-private")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "false\n")

    def test_successful_decryption_enables_private_composition(self):
        self.key()
        self.env["PROBE_EXIT"] = "0"
        result = self.run_install("--dry")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("private=true user=visitor", self.calls())
        self.assertNotIn("\nactivate", self.calls())

    def test_missing_agent_bus_source_fails_closed_after_decryption(self):
        self.key()
        self.env.update(PROBE_EXIT="0", BUS_EXIT="1")
        result = self.run_install("--check-private")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "false\n")

    def test_explicit_public_never_checks_existing_key(self):
        self.key()
        result = self.run_install("--public", "--dry")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(" eval ", self.calls())
        self.assertIn("private=false user=visitor", self.calls())

    def test_activation_preserves_identity_and_remembers_checkout(self):
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"home={self.home}", self.calls())
        self.assertTrue(self.calls().endswith("activate\n"))
        self.assertEqual(
            (self.home / "state/dotfiles/source").read_text(), f"{ROOT}\n")

    def test_sessionless_activation_uses_pinned_dbus_config(self):
        self.env.pop("DBUS_SESSION_BUS_ADDRESS")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("\ndbus\nactivate\n", self.calls())

    def test_build_failure_never_activates_or_remembers_source(self):
        self.env["BUILD_EXIT"] = "19"
        result = self.run_install()
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertNotIn("\nactivate", self.calls())
        self.assertFalse((self.home / "state/dotfiles/source").exists())

    def test_hmswitch_routes_new_user_to_remembered_checkout(self):
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.env.pop("HMSWITCH_FLAKE")
        result = subprocess.run(
            ["bash", str(ROOT / "home/config/bin/hmswitch"), "--dry"],
            env=self.env, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls().count("\nactivate"), 1)
        self.assertNotIn("william-linux", self.calls())


if __name__ == "__main__":
    unittest.main()
