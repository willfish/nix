"""Offline server-policy regressions, with no real launchd or sudo calls."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(
        name, ROOT / "system/darwin/scripts" / (name + ".py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


health, preflight, deployment = map(load, ["health", "preflight", "deploy"])


class HealthTests(unittest.TestCase):
    def test_status_does_not_leak_launchctl_environment(self):
        output = (
            "state = running\nlast exit code = 75: EX_TEMPFAIL\n"
            "EnvironmentVariables = { TOKEN = mock-sensitive-data; }"
        )
        result = health.service_status("test", "running", lambda _: (0, output))
        self.assertTrue(result["healthy"])
        self.assertNotIn("mock-sensitive-data", json.dumps(result))
        self.assertEqual(result["lastExit"], 75)

    def test_oneshot_failure_and_socket_idle_are_distinct(self):
        idle = "state = not running\nlast exit code = 1\n"
        self.assertFalse(
            health.service_status("sops", "oneshot", lambda _: (0, idle))[
                "healthy"
            ]
        )
        self.assertTrue(
            health.service_status("ssh", "socket", lambda _: (0, idle))[
                "healthy"
            ]
        )
        self.assertFalse(
            health.service_status("missing", "socket", lambda _: (113, ""))[
                "healthy"
            ]
        )

    def test_snapshot_requires_readiness_and_retains_only_numeric_metrics(self):
        def run(args):
            if args[0] == "wait":
                return 1, "private error not for logging"
            if args[0] == "/usr/bin/vm_stat":
                return 0, "Pages free: 42.\nPages active: 99.\n"
            return 0, "total = 0.00M used = 0.00M free = 0.00M"

        result = health.snapshot({"services": [], "readiness": ["wait"]}, run)
        self.assertFalse(result["healthy"])
        self.assertEqual(result["memoryPages"]["Pages free"], 42)
        self.assertEqual(result["swapUsed"], "0.00M")
        self.assertNotIn("private error", json.dumps(result))

    def test_copytruncate_preserves_open_descriptor_and_bounds_archives(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "service.log"
            with p.open("ab") as writer:
                for n in range(3):
                    writer.write(bytes([65 + n]) * 20)
                    writer.flush()
                    self.assertTrue(health.rotate(p, 10))
                writer.write(b"live")
                writer.flush()
            self.assertEqual(p.read_bytes(), b"live")
            self.assertEqual(
                p.with_name("service.log.1").read_bytes(), b"C" * 10
            )
            self.assertEqual(
                p.with_name("service.log.2").read_bytes(), b"B" * 10
            )
            self.assertEqual(len(list(Path(directory).iterdir())), 3)
            self.assertEqual(
                p.with_name("service.log.1").stat().st_mode & 0o777, 0o600
            )

    def test_missing_and_small_logs_are_unchanged_and_links_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "log"
            self.assertFalse(health.rotate(p, 10))
            p.write_text("small")
            self.assertFalse(health.rotate(p, 10))
            link = Path(directory) / "link"
            link.symlink_to(p)
            with self.assertRaises(OSError):
                health.rotate(link, 1)
            self.assertEqual(p.read_text(), "small")


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        key = self.home / ".ssh/id_ed25519"
        key.parent.mkdir()
        key.write_text("fixture, not a key")
        key.chmod(0o600)
        self.config = {
            "architecture": "arm64",
            "host": "relay",
            "user": "william",
            "home": str(self.home),
            "legacyUserJobs": ["old.user"],
            "legacySystemJobs": ["old.system"],
            "requiredFiles": [],
            "executables": [],
            "legacySystemDirectory": str(self.home / "system"),
            "systemSecrets": str(self.home / "run-secrets"),
            "installedConfig": str(self.home / "installed.json"),
        }
        self.auto = False
        self.hostname = "relay"
        self.loaded = set()
        for target, value in [("system", "Darwin"), ("machine", "arm64")]:
            p = patch.object(preflight.platform, target, return_value=value)
            p.start()
            self.addCleanup(p.stop)
        p = patch.object(
            preflight.pwd,
            "getpwnam",
            return_value=SimpleNamespace(
                pw_uid=os.getuid(), pw_dir=str(self.home)
            ),
        )
        p.start()
        self.addCleanup(p.stop)

    def run_command(self, args):
        if args[0] == "/bin/hostname":
            return SimpleNamespace(returncode=0, stdout=self.hostname)
        if args[0] == "/usr/bin/defaults":
            return SimpleNamespace(returncode=0 if self.auto else 1, stdout="")
        return SimpleNamespace(
            returncode=0 if args[-1] in self.loaded else 113, stdout=""
        )

    def test_clean_preflight_does_not_create_files(self):
        before = sorted(self.home.rglob("*"))
        self.assertEqual(preflight.problems(self.config, self.run_command), [])
        self.assertEqual(before, sorted(self.home.rglob("*")))

    def test_wrong_host_and_autologin_are_blocked(self):
        self.hostname, self.auto = "different", True
        errors = preflight.problems(self.config, self.run_command)
        self.assertEqual(len(errors), 2)

    def test_loaded_and_installed_legacy_jobs_are_blocked(self):
        self.loaded.add("system/old.system")
        path = self.home / "Library/LaunchAgents/old.user.plist"
        path.parent.mkdir(parents=True)
        path.write_text("fixture")
        self.assertEqual(
            len(preflight.problems(self.config, self.run_command)), 2
        )

    def test_broad_key_permissions_and_missing_runtime_are_blocked(self):
        (self.home / ".ssh/id_ed25519").chmod(0o644)
        self.config["requiredFiles"] = [str(self.home / "model")]
        self.assertEqual(
            len(preflight.problems(self.config, self.run_command)), 2
        )

    def test_unowned_system_secrets_are_not_taken_over(self):
        Path(self.config["systemSecrets"]).mkdir()
        self.assertTrue(preflight.problems(self.config, self.run_command))


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        # Match the CLI's canonicalization of macOS's /var alias.
        self.root = Path(self.tmp.name).resolve()
        self.target, self.old = self.root / "new", self.root / "old"
        self.target.mkdir()
        self.old.mkdir()
        self.profile, self.current = (
            self.root / "profile",
            self.root / "current",
        )
        self.profile.symlink_to(self.old)
        self.current.symlink_to(self.old)
        self.state = self.root / "state"
        self.calls = []
        self.fail = None

    def execute(self, args):
        self.calls.append(args)
        if "darwin-preflight" in args[0] and self.fail == "preflight":
            raise subprocess.CalledProcessError(1, args)
        if args[0] == deployment.NIX_ENV:
            self.profile.unlink(missing_ok=True)
            self.profile.symlink_to(args[-1])
        if "darwin-rebuild" in args[0]:
            if self.fail == "activation":
                raise subprocess.CalledProcessError(1, args)
            self.current.unlink()
            self.current.symlink_to(self.target)

    def deploy(self):
        deployment.deploy(
            self.target,
            self.profile,
            self.current,
            self.state,
            execute=self.execute,
        )

    def test_failed_preflight_does_not_register_profile_or_create_state(self):
        self.fail = "preflight"
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual(len(self.calls), 1)
        self.assertFalse(self.state.exists())
        self.assertEqual(self.profile.resolve(), self.old)

    def test_success_records_recovery_and_activates_exact_built_path(self):
        self.deploy()
        self.assertEqual(self.profile.resolve(), self.target)
        self.assertEqual(self.current.resolve(), self.target)
        record = next(self.state.glob("rollout-*/generations.json"))
        self.assertEqual(record.stat().st_mode & 0o777, 0o600)
        self.assertEqual(
            json.loads(record.read_text())["previousActive"], str(self.old)
        )

    def test_failed_activation_restores_previous_profile(self):
        self.fail = "activation"
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual(self.profile.resolve(), self.old)
        self.assertEqual(self.current.resolve(), self.old)

    def test_failed_registration_restores_a_partially_updated_profile(self):
        def execute(args):
            self.execute(args)
            if args[0] == deployment.NIX_ENV and args[-1] == str(self.target):
                raise subprocess.CalledProcessError(1, args)
        with self.assertRaises(subprocess.CalledProcessError):
            deployment.deploy(self.target, self.profile, self.current,
                              self.state, execute=execute)
        self.assertEqual(self.profile.resolve(), self.old)

    def test_external_profile_change_is_not_overwritten_on_failure(self):
        other = self.root / "other"
        other.mkdir()
        def execute(args):
            if "darwin-rebuild" in args[0]:
                self.profile.unlink()
                self.profile.symlink_to(other)
                raise subprocess.CalledProcessError(1, args)
            self.execute(args)
        with self.assertRaises(subprocess.CalledProcessError):
            deployment.deploy(self.target, self.profile, self.current,
                              self.state, execute=execute)
        self.assertEqual(self.profile.resolve(), other)

    def test_profile_race_after_success_is_rejected_without_overwrite(self):
        other = self.root / "other"
        other.mkdir()
        def execute(args):
            self.execute(args)
            if "darwin-rebuild" in args[0]:
                self.profile.unlink()
                self.profile.symlink_to(other)
        with self.assertRaisesRegex(RuntimeError, "Active system"):
            deployment.deploy(self.target, self.profile, self.current,
                              self.state, execute=execute)
        self.assertEqual(self.profile.resolve(), other)

    def test_successful_exit_without_activation_is_rejected(self):
        def execute(args):
            if "darwin-rebuild" not in args[0]:
                self.execute(args)
        with self.assertRaisesRegex(RuntimeError, "Active system"):
            deployment.deploy(self.target, self.profile, self.current,
                              self.state, execute=execute)
        self.assertEqual(self.profile.resolve(), self.old)

    def test_failed_first_install_removes_only_new_profile_pointer(self):
        self.profile.unlink()
        self.fail = "activation"
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertFalse(self.profile.is_symlink())
        self.assertTrue(self.target.exists())


class HomeReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = json.loads(
            subprocess.check_output(
                [
                    "nix",
                    "eval",
                    "--json",
                    str(ROOT)
                    + "#homeConfigurations.william-darwin"
                    + ".config.home.activation",
                    "--apply",
                    "a: { mark = a.markDarwinReady; "
                    "invalidate = a.invalidateDarwinReady; }",
                ],
                text=True,
            )
        )

    def test_marker_contains_exact_generation_and_waits_for_all_steps(self):
        with tempfile.TemporaryDirectory() as directory:
            script = self.entries["mark"]["data"].replace(
                "/Users/william/.local/state/dotfiles", directory
            )
            subprocess.run(
                ["bash", "-eu", "-c", script],
                check=True,
                env={**os.environ, "newGenPath": "/nix/store/mock-home"},
            )
            marker = Path(directory) / "home-ready"
            self.assertEqual(marker.read_text(), "/nix/store/mock-home\n")
            for step in [
                "sops-nix",
                "linkGeneration",
                "importGitSigningKey",
                "installHermesHouseTelegram",
                "onFilesChange",
            ]:
                self.assertIn(step, self.entries["mark"]["after"])

    def test_failure_after_invalidation_cannot_leave_ready_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "home-ready"
            marker.write_text("/old-home\n")
            script = (
                self.entries["invalidate"]["data"]
                + "\nfalse\n"
                + self.entries["mark"]["data"]
            ).replace("/Users/william/.local/state/dotfiles", directory)
            result = subprocess.run(
                ["bash", "-eu", "-c", script],
                env={**os.environ, "newGenPath": "/new-home"},
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
