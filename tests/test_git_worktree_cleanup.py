"""Exercise the installed Git wrapper against disposable real Git
repositories.
"""

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GitSshIdentityTest(unittest.TestCase):
    def test_git_uses_only_the_default_account_key(self):
        git = (ROOT / "home/user/git.nix").read_text()
        env = (ROOT / "home/user/environment.nix").read_text()
        self.assertIn("IdentitiesOnly=yes", git)
        self.assertIn(
            "IdentityFile=${config.home.homeDirectory}/.ssh/id_ed25519",
            git,
        )
        self.assertIn(
            "home.sessionVariables.GIT_SSH_COMMAND = gitSshCommand",
            git,
        )
        self.assertIn("systemd.user.services.ssh-add-default", git)
        self.assertIn(
            'ExecStart = "${pkgs.openssh}/bin/ssh-add"',
            git,
        )
        self.assertNotIn("GIT_SSH_COMMAND", env)


class GitWorktreeCleanupTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="git-cleanup-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.repo = self.root / "repo"
        self.tools = self.root / "tools"
        (self.tools / "bin").mkdir(parents=True)
        self.log = self.root / "cleanup.log"
        self.env = os.environ.copy()
        # Only child processes see this home and Git config.
        # Never use real state.
        self.env.update(
            HOME=str(self.home),
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_COUNT="0",
            GIT_AUTHOR_NAME="Fixture",
            GIT_AUTHOR_EMAIL="fixture@example.invalid",
            GIT_COMMITTER_NAME="Fixture",
            GIT_COMMITTER_EMAIL="fixture@example.invalid",
            CLEANUP_LOG=str(self.log),
            PATH=str(self.tools / "bin") + os.pathsep + self.env["PATH"],
        )
        for key in (
            "GIT_DIR",
            "GIT_WORK_TREE",
            "GIT_COMMON_DIR",
            "GIT_INDEX_FILE",
        ):
            self.env.pop(key, None)
        exec_path = Path(
            subprocess.check_output(
                ["git", "--exec-path"], env=self.env, text=True
            ).strip()
        )
        self.git = exec_path.parent.parent / "bin" / "git"
        if not self.git.is_file():
            self.git = Path(shutil.which("git"))
        self.write_tool(
            "pg_ctl",
            """
            printf 'pg_ctl %s\n' "$*" >> "$CLEANUP_LOG"
            exit "${FAIL_PG_STOP:-0}"
        """,
        )
        self.write_tool(
            "direnv",
            """
            printf 'direnv %s\n' "$*" >> "$CLEANUP_LOG"
        """,
        )
        source = (ROOT / "home/user/git.nix").read_text()
        matches = re.findall(
            r'gitWithWorktreeDirenv = pkgs.writeShellScriptBin "git" '
            r"\'\'\n(.*?)\n  \'\';\nin",
            source,
            re.S,
        )
        self.assertEqual(
            len(matches), 1, "extract exactly the installed wrapper"
        )
        script = textwrap.dedent(matches[0])
        script = script.replace("${pkgs.git}/bin/git", str(self.git))
        script = script.replace("${pkgs.postgresql}", str(self.tools))
        script = script.replace("''${", "${").replace("'''", "''")
        # The socket namespace is also isolated, even on accidental ID
        # collision.
        script = script.replace("/tmp/pg-", str(self.root / "pg-"))
        self.wrapper = self.root / "git-wrapper"
        self.wrapper.write_text("#!/usr/bin/env bash\n" + script)
        self.wrapper.chmod(0o755)
        self.real(
            "init", "--initial-branch=main", str(self.repo), cwd=self.root
        )
        self.real("config", "commit.gpgsign", "false")
        (self.repo / "flake.nix").write_text("{}\n")
        self.real("add", "flake.nix")
        self.real("commit", "-m", "fixture")

    def write_tool(self, name, body):
        path = self.tools / "bin" / name
        path.write_text(
            "#!/usr/bin/env bash\nset -eu\n" + textwrap.dedent(body)
        )
        path.chmod(0o755)

    def real(self, *args, cwd=None):
        return subprocess.run(
            [str(self.git), *args],
            cwd=cwd or self.repo,
            env=self.env,
            text=True,
            capture_output=True,
            check=True,
        )

    def wrapped(self, *args, cwd=None):
        return subprocess.run(
            [str(self.wrapper), *args],
            cwd=cwd or self.repo,
            env=self.env,
            text=True,
            capture_output=True,
            timeout=15,
        )

    def worktree(self, name="feature", locked=False, missing=False):
        path = self.root / name
        self.real(
            "worktree",
            "add",
            "-b",
            "branch-" + re.sub(r"\s", "-", name),
            str(path),
        )
        if locked:
            self.real(
                "worktree", "lock", "--reason", "retain fixture", str(path)
            )
        state_id = hashlib.md5((str(path) + "\n").encode()).hexdigest()[:8]
        state = self.home / ".local/share/postgres/worktrees" / state_id
        state.mkdir(parents=True)
        (state / "sentinel").write_text("fixture database\n")
        (state / "postmaster.pid").write_text(
            "synthetic PID, never read by real tools\n"
        )
        if missing:
            shutil.rmtree(path)
        return path, state

    def assert_state_retained(self, state):
        self.assertTrue((state / "sentinel").is_file())
        self.assertFalse(
            self.log.exists(), self.log.read_text() if self.log.exists() else ""
        )

    def test_locked_remove_never_cleans_state(self):
        path, state = self.worktree(locked=True)
        result = self.wrapped("worktree", "remove", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(path.is_dir())
        self.assert_state_retained(state)

    def test_dirty_remove_never_cleans_state(self):
        path, state = self.worktree()
        (path / "flake.nix").write_text("{ changed = true; }\n")
        result = self.wrapped("worktree", "remove", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assert_state_retained(state)

    def test_untracked_remove_never_cleans_state(self):
        path, state = self.worktree()
        (path / "untracked").write_text("keep me\n")
        self.assertNotEqual(
            self.wrapped("worktree", "remove", str(path)).returncode, 0
        )
        self.assert_state_retained(state)

    def test_invalid_remove_option_never_cleans_state(self):
        path, state = self.worktree()
        result = self.wrapped(
            "worktree", "remove", "--invalid-option", str(path)
        )
        self.assertNotEqual(result.returncode, 0)
        self.assert_state_retained(state)

    def test_extra_remove_argument_never_cleans_state(self):
        path, state = self.worktree()
        result = self.wrapped("worktree", "remove", str(path), "extra-argument")
        self.assertNotEqual(result.returncode, 0)
        self.assert_state_retained(state)

    def test_successful_remove_cleans_state_after_git_accepts(self):
        path, state = self.worktree()
        result = self.wrapped("worktree", "remove", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(path.exists())
        self.assertFalse(state.exists())
        self.assertIn("pg_ctl stop", self.log.read_text())
        self.assertNotIn("direnv", self.log.read_text())

    def test_successful_remove_resolves_basename_with_global_directory_option(
        self,
    ):
        path, state = self.worktree("feature with spaces")
        result = self.wrapped(
            "-C", str(self.repo), "worktree", "remove", path.name, cwd=self.root
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(state.exists())

    def test_successful_remove_handles_newline_in_path(self):
        path, state = self.worktree("feature\nline")
        result = self.wrapped("worktree", "remove", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(state.exists())

    def test_failed_postgres_stop_preserves_state(self):
        path, state = self.worktree()
        self.env["FAIL_PG_STOP"] = "1"
        result = self.wrapped("worktree", "remove", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(
            path.exists(), "Git accepted removal before cleanup failed"
        )
        self.assertTrue((state / "sentinel").is_file())
        self.assertIn("retaining", result.stderr.lower())

    def test_locked_missing_worktree_survives_prune(self):
        path, state = self.worktree(locked=True, missing=True)
        result = self.wrapped("worktree", "prune", "--expire", "now")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            str(path), self.real("worktree", "list", "--porcelain").stdout
        )
        self.assert_state_retained(state)

    def test_unexpired_missing_worktree_survives_prune(self):
        path, state = self.worktree(missing=True)
        result = self.wrapped("worktree", "prune", "--expire", "2000-01-01")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            str(path), self.real("worktree", "list", "--porcelain").stdout
        )
        self.assert_state_retained(state)

    def test_default_prune_matches_real_git(self):
        reference, _ = self.worktree("reference", missing=True)
        self.real("worktree", "prune")
        removed = (
            str(reference)
            not in self.real("worktree", "list", "--porcelain").stdout
        )
        _, state = self.worktree(missing=True)
        result = self.wrapped("worktree", "prune")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(not state.exists(), removed)

    def test_prune_dry_run_preserves_state(self):
        _, state = self.worktree(missing=True)
        result = self.wrapped(
            "worktree", "prune", "--dry-run", "--expire", "now"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_state_retained(state)

    def test_rejected_prune_never_cleans_state(self):
        _, state = self.worktree(missing=True)
        result = self.wrapped("worktree", "prune", "--invalid-option")
        self.assertNotEqual(result.returncode, 0)
        self.assert_state_retained(state)

    def test_expired_prune_cleans_only_removed_registrations(self):
        _, state = self.worktree(missing=True)
        _, locked_state = self.worktree("locked", locked=True, missing=True)
        result = self.wrapped("worktree", "prune", "--expire", "now")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(state.exists())
        self.assertTrue(locked_state.is_dir())
        self.assertEqual(self.log.read_text().count("pg_ctl stop"), 1)


if __name__ == "__main__":
    unittest.main()
