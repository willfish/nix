"""Exercise git cm using shipped Fish functions and disposable local origins."""
from pathlib import Path
import re
import subprocess
import textwrap
import unittest

import test_git_cleanup as cleanup_fixture

ROOT = Path(__file__).resolve().parents[1]


class GitCmTest(unittest.TestCase):
    def setUp(self):
        self.fixture = cleanup_fixture.GitCleanupTest()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.base = self.fixture.fixture
        self.repo = self.base.repo
        self.env = self.base.env
        self.real = self.base.real
        source = (ROOT / "home/user/shells.nix").read_text()
        body = re.findall(
            r'git-cm = pkgs.writeShellScriptBin "git-cm" '
            r"''\n(.*?)\n  '';", source, re.S,
        )
        self.assertEqual(len(body), 1)
        self.base.write_tool(
            "git-cm", textwrap.dedent(body[0]).replace("''${", "${")
        )
        self.base.write_tool(
            "git-cleanup",
            'pwd > "$CLEANUP_LOG"; exit "${CLEANUP_EXIT:-0}"',
        )
        self.functions = self.base.root / "functions.fish"
        functions = []
        for name in ["__git_worktree_path_for_branch", "git"]:
            body = re.findall(
                rf"      {name} = ''\n(.*?)\n      '';", source, re.S
            )
            self.assertEqual(len(body), 1)
            functions.append(
                f"function {name}\n{textwrap.dedent(body[0])}\nend\n"
            )
        self.functions.write_text("\n".join(functions))

    def run_cm(self, cwd=None, command="git cm"):
        return subprocess.run(
            [
                "fish", "--no-config", "-c",
                'source "$argv[1]"; or exit $status; ' + command +
                '; set -l result $status; pwd; exit $result',
                str(self.functions),
            ],
            cwd=cwd or self.repo, env=self.env,
            text=True, capture_output=True, timeout=15,
        )

    def assert_destination(self, result, path):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines()[-1], str(path.resolve()))
        self.assertEqual(self.base.log.read_text().strip(), str(path.resolve()))

    def make_master_default(self):
        self.real("branch", "-m", "master")
        self.real("push", "-u", "origin", "master")
        self.real(
            "--git-dir=" + str(self.base.root / "origin.git"),
            "symbolic-ref", "HEAD", "refs/heads/master",
        )
        self.real("symbolic-ref", "--delete", "refs/remotes/origin/HEAD")

    def test_main_in_another_worktree_moves_the_calling_shell(self):
        path, _ = self.base.worktree("feature with spaces")
        self.assert_destination(self.run_cm(path), self.repo)
        self.assertTrue(path.is_dir())

    def test_default_worktree_destination_can_contain_spaces(self):
        self.real("switch", "-c", "feature")
        destination = self.base.root / "main worktree"
        self.real("worktree", "add", str(destination), "main")
        self.assert_destination(self.run_cm(), destination)

    def test_default_branch_can_exist_only_on_origin(self):
        self.real("switch", "-c", "feature")
        self.real("branch", "-d", "main")
        self.assert_destination(self.run_cm(), self.repo)
        self.assertEqual(
            self.real("branch", "--show-current").stdout.strip(), "main"
        )

    def test_standalone_command_discovers_master_in_current_worktree(self):
        self.make_master_default()
        self.real("switch", "-c", "feature")
        self.assert_destination(
            self.run_cm(command="command git cm"), self.repo
        )
        self.assertEqual(
            self.real("branch", "--show-current").stdout.strip(), "master"
        )

    def test_missing_default_ref_discovers_master_not_main(self):
        self.make_master_default()
        path, _ = self.base.worktree()
        self.assert_destination(self.run_cm(path), self.repo)
        result = self.real("symbolic-ref", "refs/remotes/origin/HEAD")
        self.assertEqual(result.stdout.strip(), "refs/remotes/origin/master")

    def test_missing_default_ref_discovers_main(self):
        self.real("symbolic-ref", "--delete", "refs/remotes/origin/HEAD")
        path, _ = self.base.worktree()
        self.assert_destination(self.run_cm(path), self.repo)

    def test_stale_default_ref_is_repaired(self):
        self.real(
            "symbolic-ref", "refs/remotes/origin/HEAD",
            "refs/remotes/origin/missing",
        )
        self.assert_destination(self.run_cm(), self.repo)

    def test_nonstandard_remote_default_is_supported(self):
        self.real("branch", "-m", "trunk")
        self.real("push", "-u", "origin", "trunk")
        self.real(
            "--git-dir=" + str(self.base.root / "origin.git"),
            "symbolic-ref", "HEAD", "refs/heads/trunk",
        )
        self.real("symbolic-ref", "--delete", "refs/remotes/origin/HEAD")
        self.real("switch", "-c", "feature")
        self.assert_destination(self.run_cm(), self.repo)
        self.assertEqual(
            self.real("branch", "--show-current").stdout.strip(), "trunk"
        )

    def test_dirty_feature_worktree_is_left_intact(self):
        path, _ = self.base.worktree()
        changed = path / "flake.nix"
        changed.write_text("local changes\n")
        self.assert_destination(self.run_cm(path), self.repo)
        self.assertEqual(changed.read_text(), "local changes\n")

    def test_conflicting_changes_block_checkout_and_cleanup(self):
        self.real("switch", "-c", "feature")
        (self.repo / "flake.nix").write_text("feature commit\n")
        self.real("commit", "-am", "feature")
        (self.repo / "flake.nix").write_text("uncommitted\n")
        result = self.run_cm()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.base.log.exists())
        self.assertEqual(
            (self.repo / "flake.nix").read_text(), "uncommitted\n"
        )

    def test_missing_origin_fails_before_checkout_or_cleanup(self):
        self.real("remote", "remove", "origin")
        self.real("switch", "-c", "feature")
        result = self.run_cm()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.base.log.exists())
        self.assertEqual(
            self.real("branch", "--show-current").stdout.strip(), "feature"
        )

    def test_cleanup_failure_is_returned(self):
        self.env["CLEANUP_EXIT"] = "17"
        self.assertEqual(self.run_cm().returncode, 17)

    def test_existing_switch_and_checkout_navigation_is_preserved(self):
        path, _ = self.base.worktree()
        for command in ["git switch main", "git checkout main"]:
            with self.subTest(command=command):
                result = self.run_cm(path, command)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout.splitlines()[-1], str(self.repo.resolve())
                )
                self.assertFalse(self.base.log.exists())


if __name__ == "__main__":
    unittest.main()
