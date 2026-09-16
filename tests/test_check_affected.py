"""Selection, push protocol and immutable-source push gate regressions."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_affected", ROOT / "scripts/check-affected.py"
)
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)
ZERO = "0" * 40


class SelectionTests(unittest.TestCase):
    def test_each_linux_host_is_isolated(self):
        for host in gate.LINUX_HOSTS:
            with self.subTest(host=host):
                selected = gate.select([f"system/{host}/configuration.nix"])
                self.assertEqual(selected.systems, {host})
                self.assertFalse(selected.homes)
                self.assertEqual(selected.checks, {"pre-commit"})

    def test_shared_modules(self):
        for module, expected in [
            ("workstation", gate.WORKSTATIONS),
            ("server", {"terminus"}),
            ("base", gate.LINUX_HOSTS),
            ("tailscale", gate.LINUX_HOSTS),
        ]:
            with self.subTest(module=module):
                self.assertEqual(
                    gate.select([f"system/modules/{module}.nix"]).systems,
                    expected,
                )

    def test_darwin_is_isolated(self):
        selected = gate.select(["system/darwin/relay.nix"])
        self.assertEqual(selected.systems, {"relay"})
        self.assertEqual(selected.homes, {"william@relay"})

    def test_home_changes_cover_profiles_and_embedded_darwin_home(self):
        selected = gate.select(["home/config/fish/config.fish"])
        self.assertEqual(len(selected.homes), 6)
        self.assertEqual(selected.systems, {"relay"})
        self.assertIn("host-capabilities", selected.checks)

    def test_flake_lock_and_unknown_changes_fail_closed(self):
        for path in [
            "flake.nix",
            "flake.lock",
            "new-module.nix",
            "scripts/check-affected.py",
        ]:
            with self.subTest(path=path):
                selected = gate.select([path])
                self.assertEqual(selected.systems, gate.LINUX_HOSTS | {"relay"})
                self.assertEqual(len(selected.homes), 6)

    def test_docs_only_skip_but_home_markdown_is_deployed(self):
        selected = gate.select(["docs/voice.md", "AGENTS.md", "README.md"])
        self.assertEqual(selected, gate.Selection())
        self.assertTrue(gate.select(["home/config/llm/AGENTS.md"]).homes)

    def test_native_builds_cross_platform_evaluations(self):
        selected = gate.select(["flake.nix"])
        for native in [gate.LINUX, gate.DARWIN]:
            evaluate, build = gate.targets(selected, native)
            self.assertEqual(len(evaluate), 11)
            self.assertEqual(len(evaluate), len(set(evaluate)))
            self.assertEqual(
                any("nixosConfigurations" in t for t in build),
                native == gate.LINUX,
            )
            self.assertEqual(
                any("darwinConfigurations" in t for t in build),
                native == gate.DARWIN,
            )
            self.assertEqual(
                any("headless-browser" in t for t in build),
                native == gate.DARWIN,
            )

    def test_build_failure_propagates(self):
        with patch.object(
            gate.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(1, "nix"),
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                gate.run_checks(
                    ROOT,
                    gate.select(["system/terminus/storage.nix"]),
                    gate.LINUX,
                )


class GitTests(unittest.TestCase):
    def setUp(self):
        hostname = patch.object(
            gate.socket, "gethostname", return_value="andromeda"
        )
        hostname.start()
        self.addCleanup(hostname.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.previous = Path.cwd()
        os.chdir(self.repo)
        self.addCleanup(os.chdir, self.previous)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Hook Test")
        self.git("config", "user.email", "hook@example.invalid")
        self.base = self.commit("README.md", "base\n")
        self.remote = self.root / "remote.git"
        self.git("clone", "-q", "--bare", str(self.repo), str(self.remote))
        self.git("remote", "add", "origin", str(self.remote))

    def git(self, *args):
        return gate.git(*args)

    def commit(self, path, text):
        file = self.repo / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
        self.git("add", ".")
        self.git("-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def line(self, new, old=ZERO, ref="main"):
        return f"refs/heads/{ref} {new} refs/heads/{ref} {old}"

    def test_existing_push_ignores_dirty_worktree(self):
        new = self.commit("system/terminus/storage.nix", "committed\n")
        (self.repo / "flake.nix").write_text("dirty\n")
        result = gate.revisions([self.line(new, self.base)], "origin")
        self.assertEqual(result, {new: ["system/terminus/storage.nix"]})

    def test_new_branch_uses_actual_remote_not_tracking_refs(self):
        new = self.commit("system/foundation/configuration.nix", "foundation\n")
        self.git("update-ref", "refs/remotes/origin/main", new)
        self.assertEqual(
            gate.revisions([self.line(new)], "origin"),
            {new: ["system/foundation/configuration.nix"]},
        )

    def test_first_push_to_empty_remote_includes_root(self):
        empty = self.root / "empty.git"
        self.git("init", "-q", "--bare", str(empty))
        self.assertEqual(
            gate.revisions([self.line(self.base)], str(empty)),
            {self.base: ["README.md"]},
        )

    def test_deletion_does_not_query_remote(self):
        with patch.object(gate, "remote_commits", side_effect=AssertionError):
            self.assertEqual(
                gate.revisions([self.line(ZERO, self.base)], "origin"), {}
            )

    def test_multiple_refs_and_duplicate_tips(self):
        first = self.commit("system/terminus/storage.nix", "server\n")
        second = self.commit("system/starfish/configuration.nix", "laptop\n")
        result = gate.revisions(
            [
                self.line(first, self.base),
                self.line(second, first, "other"),
                self.line(second, self.base, "duplicate"),
            ],
            "origin",
        )
        self.assertEqual(set(result), {first, second})
        self.assertEqual(
            result[second],
            [
                "system/starfish/configuration.nix",
                "system/terminus/storage.nix",
            ],
        )

    def test_force_push_rename_and_deletion(self):
        old = self.commit("system/terminus/old file.nix", "old\n")
        self.git("reset", "--hard", self.base)
        new = self.commit("system/starfish/new file.nix", "new\n")
        self.assertEqual(
            gate.revisions([self.line(new, old)], "origin")[new],
            ["system/starfish/new file.nix", "system/terminus/old file.nix"],
        )

    def test_annotated_tag_is_peeled(self):
        self.git("tag", "-am", "release", "release")
        tag = self.git("rev-parse", "release")
        self.assertEqual(
            gate.revisions([self.line(tag)], "origin"), {self.base: []}
        )

    def test_missing_old_object_blocks(self):
        with self.assertRaises(subprocess.CalledProcessError):
            gate.revisions([self.line(self.base, "1" * 40)], "origin")

    def test_snapshot_is_pushed_commit_and_is_cleaned(self):
        new = self.commit("system/terminus/storage.nix", "committed\n")
        (self.repo / "system/terminus/storage.nix").write_text("dirty\n")
        snapshots = []

        def inspect(root, selection, native):
            snapshots.append(root)
            self.assertEqual(
                (root / "system/terminus/storage.nix").read_text(),
                "committed\n",
            )
            self.assertEqual(selection.systems, {"terminus"})
            self.assertEqual(native, gate.LINUX)
            raise RuntimeError("check failed")

        with patch.object(gate, "run_checks", side_effect=inspect):
            with self.assertRaisesRegex(RuntimeError, "check failed"):
                gate.check_revision(
                    new,
                    gate.select(["system/terminus/storage.nix"]),
                    gate.LINUX,
                )
        self.assertFalse(snapshots[0].exists())
        self.assertEqual(
            (self.repo / "system/terminus/storage.nix").read_text(), "dirty\n"
        )

    def test_install_is_idempotent_and_protects_existing_hook(self):
        first = self.root / "one" / gate.MARKER
        second = self.root / "two" / gate.MARKER
        gate.install(first)
        gate.install(first)
        gate.install(second)
        hook = Path(self.git("rev-parse", "--git-path", "hooks/pre-push"))
        self.assertEqual(os.readlink(hook), str(second))
        hook.unlink()
        hook.write_text("#!/bin/sh\nexit 1\n")
        with self.assertRaisesRegex(ValueError, "unmanaged"):
            gate.install(first)
        self.assertFalse(hook.is_symlink())

    def test_other_hosts_remove_only_managed_hooks(self):
        executable = self.root / gate.MARKER
        hook = Path(self.git("rev-parse", "--git-path", "hooks/pre-push"))
        for host in ("terminus", "foundation", "starfish", "relay"):
            gate.install(executable)
            with patch.object(gate.socket, "gethostname", return_value=host):
                gate.install(executable)
                self.assertFalse(hook.is_symlink())
                hook.write_text("#!/bin/sh\nexit 1\n")
                gate.install(executable)
                self.assertEqual(hook.read_text(), "#!/bin/sh\nexit 1\n")
            hook.unlink()

    def test_other_hosts_skip_before_git_stdin_or_nix(self):
        for host in ("terminus", "foundation", "starfish", "relay"):
            with (
                patch.object(gate.socket, "gethostname", return_value=host),
                patch.object(sys, "argv", ["check-affected"]),
                patch.object(gate, "git", side_effect=AssertionError),
                patch.object(gate, "revisions", side_effect=AssertionError),
                patch.object(gate, "run_checks", side_effect=AssertionError),
            ):
                self.assertEqual(gate.main(), 0)

    def test_andromeda_hostname_accepts_fqdn(self):
        with patch.object(
            gate.socket, "gethostname", return_value="Andromeda.fritz.box"
        ):
            self.assertTrue(gate.automatic_gate_enabled())

    def test_manual_build_rejects_untracked_sources(self):
        (self.repo / "flake.nix").write_text("untracked\n")
        with (
            patch.object(gate.socket, "gethostname", return_value="terminus"),
            patch.object(sys, "argv", ["check-affected", "--base", "HEAD"]),
        ):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(gate.main(), 1)

    def test_git_push_checks_snapshot_and_blocks_failed_build(self):
        new = self.commit("system/terminus/storage.nix", "committed\n")
        (self.repo / "system/terminus/storage.nix").write_text("dirty\n")
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        log = self.root / "nix.jsonl"
        nix = bin_dir / "nix"
        nix.write_text(
            f"#!{sys.executable}\n"
            + """import json, os, pathlib, sys
args = sys.argv[1:]
record = {"args": args}
for arg in args:
    if arg.startswith("path:"):
        root = pathlib.Path(arg[5:].split("#")[0])
        record["source"] = (root / "system/terminus/storage.nix").read_text()
with open(os.environ["NIX_TEST_LOG"], "a") as stream:
    stream.write(json.dumps(record) + "\\n")
if "builtins.currentSystem" in args:
    print("x86_64-linux")
if args[0] == "build" and os.environ.get("NIX_TEST_FAIL"):
    sys.exit(42)
"""
        )
        nix.chmod(0o755)
        executable = bin_dir / gate.MARKER
        executable.write_text(
            f"#!{sys.executable}\nimport runpy, socket\n"
            "socket.gethostname = lambda: 'andromeda'\n"
            f"runpy.run_path({str(ROOT / 'scripts/check-affected.py')!r}, "
            "run_name='__main__')\n"
        )
        executable.chmod(0o755)
        gate.install(executable)
        env = dict(
            os.environ,
            PATH=f"{bin_dir}:{os.environ['PATH']}",
            NIX_TEST_LOG=str(log),
            NIX_TEST_FAIL="1",
        )
        failed = subprocess.run(
            ["git", "push", "origin", "main"],
            env=env,
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        self.assertIn("blocked", failed.stderr)
        self.assertEqual(
            self.git("--git-dir", str(self.remote), "rev-parse", "main"),
            self.base,
        )
        records = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertTrue(any(r["args"][0] == "build" for r in records))
        self.assertTrue(
            all(r["source"] == "committed\n" for r in records if "source" in r)
        )
        self.assertFalse(any("andromeda" in str(r) for r in records))
        env.pop("NIX_TEST_FAIL")
        passed = subprocess.run(
            ["git", "push", "origin", "main"],
            env=env,
            text=True,
            capture_output=True,
        )
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertEqual(
            self.git("--git-dir", str(self.remote), "rev-parse", "main"), new
        )
        self.assertEqual(
            (self.repo / "system/terminus/storage.nix").read_text(), "dirty\n"
        )

    def test_manual_plan_covers_untracked_changes(self):
        (self.repo / "flake.nix").write_text("untracked\n")
        output = io.StringIO()
        with patch.object(
            sys, "argv", ["check-affected", "--base", "HEAD", "--plan"]
        ):
            with contextlib.redirect_stdout(output):
                self.assertEqual(gate.main(), 0)
        self.assertIn("andromeda", output.getvalue())
        self.assertIn("relay", output.getvalue())


if __name__ == "__main__":
    unittest.main()
