"""Check cleanup policy with local origins and the actual installed Git
wrapper.
"""

from pathlib import Path
import re
import shutil
import subprocess
import textwrap
import unittest

import test_git_worktree_cleanup as wrapper_fixture


ROOT = Path(__file__).resolve().parents[1]


class GitCleanupTest(unittest.TestCase):
    def setUp(self):
        self.fixture = wrapper_fixture.GitWorktreeCleanupTest()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.real = self.fixture.real
        self.root = self.fixture.root
        self.repo = self.fixture.repo
        self.env = self.fixture.env
        origin = self.root / "origin.git"
        self.real("init", "--bare", "--initial-branch=main", str(origin))
        self.real("remote", "add", "origin", str(origin))
        self.real("push", "-u", "origin", "main")
        self.real(
            "symbolic-ref",
            "refs/remotes/origin/HEAD",
            "refs/remotes/origin/main",
        )
        source = (ROOT / "home/user/shells.nix").read_text()
        matches = re.findall(
            r'git-cleanup = pkgs.writeShellScriptBin "git-cleanup" '
            r"\'\'\n(.*?)\n  \'\';",
            source,
            re.S,
        )
        self.assertEqual(
            len(matches), 1, "extract exactly the shipped cleanup command"
        )
        script = textwrap.dedent(matches[0]).replace("''${", "${")
        script = script.replace("${pkgs.git}/bin/git", str(self.fixture.git))
        self.cleanup = self.root / "git-cleanup"
        self.cleanup.write_text("#!/usr/bin/env bash\n" + script)
        self.cleanup.chmod(0o755)
        (self.fixture.tools / "bin" / "git").symlink_to(self.fixture.wrapper)

    def run_cleanup(self, cwd=None):
        return subprocess.run(
            [str(self.cleanup)],
            cwd=cwd or self.repo,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def tracked_tree(self, name="feature", merged=True, gone=True):
        path, state = self.fixture.worktree(name)
        branch = "branch-" + re.sub(r"\s", "-", name)
        if not merged:
            (path / "feature.txt").write_text("unmerged work\n")
            self.real("add", "feature.txt", cwd=path)
            self.real("commit", "-m", "unmerged fixture", cwd=path)
        self.real("push", "-u", "origin", branch)
        if gone:
            self.real("push", "origin", "--delete", branch)
        return path, state, branch

    def assert_retained(self, path, state, branch):
        self.assertTrue(path.is_dir(), f"worktree removed: {path}")
        self.assertTrue((state / "sentinel").is_file())
        self.real("show-ref", "--verify", "refs/heads/" + branch)
        self.assertFalse(
            self.fixture.log.exists(), "protected tree reached service cleanup"
        )

    def test_unpublished_merged_worktree_is_retained(self):
        path, state = self.fixture.worktree()
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_retained(path, state, "branch-feature")

    def test_unpublished_unmerged_worktree_is_retained(self):
        path, state = self.fixture.worktree()
        (path / "unmerged").write_text("commit only exists here\n")
        self.real("add", "unmerged", cwd=path)
        self.real("commit", "-m", "unpublished", cwd=path)
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_retained(path, state, "branch-feature")

    def test_gone_upstream_unmerged_worktree_is_retained(self):
        path, state, branch = self.tracked_tree(merged=False)
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_retained(path, state, branch)

    def test_existing_upstream_worktree_is_retained(self):
        path, state, branch = self.tracked_tree(gone=False)
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_differently_named_existing_upstream_is_retained(self):
        path, state = self.fixture.worktree()
        branch = "branch-feature"
        self.real("push", "-u", "origin", branch + ":remote-feature")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_safely_merged_gone_upstream_is_removed(self):
        path, state, branch = self.tracked_tree()
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(path.exists())
        self.assertFalse(state.exists())
        branches = self.real(
            "for-each-ref", "--format=%(refname)", "refs/heads"
        ).stdout
        self.assertNotIn("refs/heads/" + branch, branches)

    def test_dirty_tracked_worktree_is_retained(self):
        path, state, branch = self.tracked_tree()
        (path / "flake.nix").write_text("{ dirty = true; }\n")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_untracked_worktree_file_is_retained(self):
        path, state, branch = self.tracked_tree()
        (path / "untracked").write_text("local state\n")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_ignored_direnv_file_is_retained(self):
        path, state, branch = self.tracked_tree()
        (self.repo / ".git/info/exclude").write_text(".envrc\n")
        (path / ".envrc").write_text("exit 99\n")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_locked_worktree_is_retained(self):
        path, state, branch = self.tracked_tree()
        self.real("worktree", "lock", str(path))
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)

    def test_unavailable_locked_worktree_state_is_retained(self):
        path, state, branch = self.tracked_tree()
        self.real("worktree", "lock", str(path))
        shutil.rmtree(path)
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((state / "sentinel").is_file())
        self.real("show-ref", "--verify", "refs/heads/" + branch)
        self.assertFalse(self.fixture.log.exists())

    def test_unavailable_unpublished_worktree_keeps_service_state(self):
        path, state = self.fixture.worktree(missing=True)
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((state / "sentinel").is_file())
        self.real("show-ref", "--verify", "refs/heads/branch-feature")
        self.assertFalse(self.fixture.log.exists())

    def test_missing_origin_default_stops_before_cleanup(self):
        path, state, branch = self.tracked_tree()
        self.real(
            "symbolic-ref",
            "refs/remotes/origin/HEAD",
            "refs/remotes/origin/unknown",
        )
        result = self.run_cleanup()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("default", result.stderr.lower())
        self.assert_retained(path, state, branch)

    def test_current_worktree_is_not_removed(self):
        path, state, branch = self.tracked_tree()
        # Pull must remain possible without recreating the gone origin ref.
        (self.fixture.tools / "bin" / "git").unlink()
        self.fixture.write_tool(
            "git",
            """
            case "$*" in
              'pull --ff-only') exit 0 ;;
            esac
            exec "$WRAPPED_GIT" "$@"
        """,
        )
        self.env["WRAPPED_GIT"] = str(self.fixture.wrapper)
        result = self.run_cleanup(cwd=path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_retained(path, state, branch)

    def test_unpublished_branch_without_worktree_is_retained(self):
        self.real("branch", "unpublished")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.real("show-ref", "--verify", "refs/heads/unpublished")

    def test_cleanup_from_feature_uses_default_ancestry(self):
        path, state, branch = self.tracked_tree(merged=False, gone=False)
        self.real("branch", "local-copy", branch)
        result = self.run_cleanup(cwd=path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_retained(path, state, branch)
        self.real("show-ref", "--verify", "refs/heads/local-copy")

    def test_newline_path_is_preserved_when_ignored_files_exist(self):
        path, state, branch = self.tracked_tree("feature\nline")
        (self.repo / ".git/info/exclude").write_text(".envrc\n")
        (path / ".envrc").write_text("exit 99\n")
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_retained(path, state, branch)


if __name__ == "__main__":
    unittest.main()
