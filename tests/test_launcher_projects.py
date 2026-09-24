"""Bounded Elephant project discovery and launch arguments."""

from io import StringIO
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "launcher_projects", ROOT / "home/config/launcher/projects.py"
)
projects = importlib.util.module_from_spec(spec)
spec.loader.exec_module(projects)


class ProjectCatalogueTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name) / "home"
        self.home.mkdir()
        self.outside = Path(self.tmp.name) / "outside"
        self.outside.mkdir()
        (self.outside / "secret-marker").write_text("sops-secret-marker")
        (self.outside / ".git").mkdir()

    def repo(
        self, relative, marker="dir", git_text="gitdir: /outside/gitdir\n"
    ):
        path = self.home / relative
        path.mkdir(parents=True)
        git = path / ".git"
        if marker == "dir":
            git.mkdir()
        elif marker == "file":
            git.write_text(git_text)
        elif marker == "symlink":
            git.symlink_to(self.outside / ".git")
        else:
            raise AssertionError(marker)
        return path

    def entries(self, home=None):
        return projects.catalogue_entries(home or self.home)

    def by_subtext(self, home=None):
        return {entry["Subtext"]: entry for entry in self.entries(home)}

    def test_list_shape_dotfiles_repos_and_stable_ids(self):
        dotfiles = self.home / ".dotfiles"
        dotfiles.mkdir()
        alpha = self.repo("Repositories/zeta")
        beta = self.repo("Repositories/org/alpha")
        listed = self.entries()
        self.assertEqual(
            [entry["Subtext"] for entry in listed],
            [".dotfiles", "Repositories/org/alpha", "Repositories/zeta"],
        )
        for entry, path in (
            (listed[0], dotfiles),
            (listed[1], beta),
            (listed[2], alpha),
        ):
            self.assertEqual(
                set(entry),
                {"Text", "Subtext", "Value", "Icon", "Keywords"},
            )
            self.assertEqual(entry["Icon"], "folder")
            self.assertEqual(entry["Text"], path.name)
            self.assertEqual(entry["Value"], projects.project_id(path))
            self.assertNotIn(str(path), entry["Value"])
            self.assertEqual(len(entry["Value"]), 64)
            self.assertIsInstance(entry["Keywords"], list)
        self.assertEqual(listed[0]["Keywords"][0], ".dotfiles")
        self.assertIn("dotfiles", listed[0]["Keywords"])
        self.assertEqual(self.entries(), listed)

    def test_worktree_git_file_is_a_project_and_is_not_read(self):
        worktree = self.repo("Repositories/app-wt", marker="file")
        envrc = worktree / ".envrc"
        envrc.write_text("echo pwned > /tmp/launcher-projects-pwned\n")
        envrc.chmod(0o755)
        opened = []
        real_open = open

        def guard(file, *args, **kwargs):
            name = os.fspath(file)
            if name.endswith(".git") or name.endswith(".envrc"):
                raise AssertionError(f"opened {name}")
            return real_open(file, *args, **kwargs)

        def forbidden(*args, **kwargs):
            raise AssertionError(f"subprocess during discovery: {args}")

        with (
            patch("builtins.open", guard),
            patch.object(projects.subprocess, "Popen", forbidden),
            patch.object(projects.subprocess, "run", forbidden),
            patch.object(projects.subprocess, "call", forbidden),
        ):
            found = self.by_subtext()
        self.assertIn("Repositories/app-wt", found)
        self.assertNotIn("gitdir:", json.dumps(self.entries()))
        self.assertNotIn("/outside/gitdir", json.dumps(self.entries()))

    def test_symlink_git_marker_hidden_and_dependency_dirs_are_skipped(self):
        self.repo("Repositories/linked-marker", marker="symlink")
        hidden = self.repo("Repositories/.hidden/repo")
        escaped = self.home / "Repositories"
        escaped.mkdir(exist_ok=True)
        (escaped / "escape").symlink_to(self.outside)
        nested = escaped / "real"
        nested.mkdir()
        (nested / "hop").symlink_to(self.outside)
        self.repo("Repositories/node_modules/pkg")
        self.repo("Repositories/vendor/pkg")
        self.repo("Repositories/build/pkg")
        self.repo("Repositories/my-app")
        (escaped / "loop").symlink_to(escaped)
        found = self.by_subtext()
        self.assertEqual(list(found), ["Repositories/my-app"])
        rendered = json.dumps(found)
        self.assertNotIn("sops-secret-marker", rendered)
        self.assertNotIn(str(self.outside), rendered)
        self.assertNotIn(str(hidden), rendered)
        self.assertFalse((self.home / "Repositories").is_symlink())

    def test_symlink_repositories_and_dotfiles_are_not_followed(self):
        (self.home / ".dotfiles").symlink_to(self.outside)
        (self.home / "Repositories").symlink_to(self.outside)
        self.assertEqual(self.entries(), [])
        rendered = json.dumps(self.entries())
        self.assertNotIn("sops-secret-marker", rendered)
        self.assertNotIn(str(self.outside), rendered)

    def test_depth_limit_and_repo_boundary(self):
        included = self.repo("Repositories/a/b/c")
        self.repo("Repositories/x/y/z/too-deep")
        parent = self.repo("Repositories/parent")
        child = parent / "child"
        child.mkdir()
        (child / ".git").mkdir()
        found = self.by_subtext()
        self.assertIn("Repositories/a/b/c", found)
        self.assertEqual(
            found["Repositories/a/b/c"]["Value"], projects.project_id(included)
        )
        self.assertNotIn("Repositories/x/y/z/too-deep", found)
        self.assertIn("Repositories/parent", found)
        self.assertNotIn("Repositories/parent/child", found)
        self.assertEqual(projects.MAX_DEPTH, 3)
        self.assertEqual(projects.MAX_ENTRIES, 200)
        self.assertEqual(projects.MAX_DIR_ENTRIES, 4000)

    def test_entry_limit_is_stable(self):
        (self.home / ".dotfiles").mkdir()
        for number in range(201):
            self.repo(f"Repositories/p{number:03d}")
        listed = self.entries()
        self.assertEqual(len(listed), 200)
        self.assertEqual(listed[0]["Subtext"], ".dotfiles")
        self.assertEqual(listed[1]["Subtext"], "Repositories/p000")
        self.assertEqual(listed[-1]["Subtext"], "Repositories/p198")
        self.assertNotIn(
            "Repositories/p199", [item["Subtext"] for item in listed]
        )

    def test_project_limit_stops_further_traversal(self):
        self.repo("Repositories/a/inner")
        self.repo("Repositories/b")
        self.repo("Repositories/c")
        opened = []
        real_scandir = projects.os.scandir

        def tracking(path):
            opened.append(Path(path).name)
            return real_scandir(path)

        with (
            patch.object(projects, "MAX_ENTRIES", 1),
            patch.object(projects.os, "scandir", tracking),
        ):
            found = projects.discover_projects(self.home)
        self.assertEqual([path.name for path in found], ["inner"])
        self.assertEqual(found[0], self.home / "Repositories/a/inner")
        self.assertEqual(
            projects.project_id(found[0]),
            projects.project_id(self.home / "Repositories/a/inner"),
        )
        self.assertIn("a", opened)
        self.assertNotIn("b", opened)
        self.assertNotIn("c", opened)

    def test_dir_entry_budget_stops_without_reading_the_rest(self):
        for number in range(12):
            self.repo(f"Repositories/p{number:02d}")
        reads = []
        real_scandir = projects.os.scandir

        class Tracking:
            def __init__(self, path):
                self._iterator = real_scandir(path)

            def __iter__(self):
                return self

            def __next__(self):
                entry = next(self._iterator)
                reads.append(entry.name)
                return entry

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.close()

            def close(self):
                self._iterator.close()

        with (
            patch.object(projects, "MAX_DIR_ENTRIES", 3),
            patch.object(projects.os, "scandir", lambda path: Tracking(path)),
        ):
            found = projects.discover_projects(self.home)
        self.assertLessEqual(len(reads), 3)
        self.assertLessEqual(len(found), 3)
        self.assertLess(len(found), 12)
        self.assertTrue(
            all(projects._valid_id(projects.project_id(path)) for path in found)
        )

    def test_malicious_names_stay_data(self):
        name = "proj; rm -rf ~ $(id) 'quote' \"dq\" --flag"
        path = self.repo(Path("Repositories") / name)
        entry = self.by_subtext()[f"Repositories/{name}"]
        self.assertEqual(entry["Text"], name)
        self.assertEqual(entry["Value"], projects.project_id(path))
        self.assertNotIn(str(path), entry["Value"])
        encoded = json.dumps([entry], ensure_ascii=False)
        parsed = json.loads(encoded)
        self.assertEqual(parsed[0]["Text"], name)
        argv = projects.launch_argv("terminal", path)
        self.assertEqual(argv, ["ghostty", f"--working-directory={path}"])
        self.assertEqual(len(argv), 2)
        editor = projects.launch_argv("editor", path)
        self.assertEqual(
            editor,
            ["ghostty", f"--working-directory={path}", "-e", "nvim", "."],
        )
        files = projects.launch_argv("files", path)
        self.assertEqual(files, ["xdg-open", str(path)])
        calls = []

        def popen(argv, **kwargs):
            calls.append((argv, kwargs))
            return object()

        with patch.object(projects.subprocess, "Popen", popen):
            projects.spawn(argv)
        wrapped, kwargs = calls[0]
        self.assertEqual(
            wrapped,
            list(projects.SYSTEMD_RUN_PREFIX) + argv,
        )
        self.assertEqual(wrapped[wrapped.index("--") + 1 :], argv)
        self.assertEqual(len(wrapped), len(projects.SYSTEMD_RUN_PREFIX) + 2)
        self.assertIs(kwargs["shell"], False)
        self.assertNotIn(";", wrapped)
        self.assertNotIn("$(id)", wrapped)
        self.assertTrue(any(";" in argument for argument in wrapped))
        self.assertTrue(any("$(id)" in argument for argument in wrapped))
        self.assertTrue(any("'quote'" in argument for argument in wrapped))

    def test_cli_list_uses_home_flag_or_env_and_prints_json(self):
        self.repo("Repositories/app")
        output = StringIO()
        with patch.object(projects.sys, "stdout", output):
            code = projects.main(["--home", str(self.home), "list"])
        self.assertEqual(code, 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(payload[0]["Subtext"], "Repositories/app")
        output = StringIO()
        with (
            patch.dict(
                os.environ, {"LAUNCHER_HOME": str(self.home)}, clear=False
            ),
            patch.object(projects.sys, "stdout", output),
        ):
            code = projects.main(["list"])
        self.assertEqual(code, 0)
        self.assertEqual(
            json.loads(output.getvalue())[0]["Value"],
            projects.project_id(self.home / "Repositories/app"),
        )

    def test_vanished_and_unknown_ids_do_not_launch_or_log_paths(self):
        path = self.repo("Repositories/gone")
        identifier = projects.project_id(path)
        path.rename(self.outside / "gone")
        errors = StringIO()
        with (
            patch.object(projects.sys, "stderr", errors),
            patch.object(
                projects.subprocess,
                "Popen",
                side_effect=AssertionError("launched"),
            ),
        ):
            code = projects.main(
                ["--home", str(self.home), "open", identifier, "terminal"]
            )
        self.assertEqual(code, 1)
        self.assertEqual(errors.getvalue().strip(), "unknown project")
        self.assertNotIn(str(self.home), errors.getvalue())
        self.assertNotIn("sops-secret-marker", errors.getvalue())
        errors = StringIO()
        with (
            patch.object(projects.sys, "stderr", errors),
            patch.object(
                projects.subprocess,
                "Popen",
                side_effect=AssertionError("launched"),
            ),
        ):
            code = projects.main(
                ["--home", str(self.home), "open", str(self.outside), "files"]
            )
        self.assertEqual(code, 1)
        self.assertNotIn(str(self.outside), errors.getvalue())
        self.assertNotIn("sops-secret-marker", errors.getvalue())

    def test_open_rescans_and_uses_detached_argument_lists(self):
        path = self.repo("Repositories/app", marker="file")
        (path / ".envrc").write_text("export DIRENV_SHOULD_NOT_RUN=1\n")
        identifier = projects.project_id(path)
        calls = []

        def popen(argv, **kwargs):
            calls.append((argv, kwargs))
            return object()

        for action, expected in (
            ("terminal", ["ghostty", f"--working-directory={path}"]),
            (
                "editor",
                ["ghostty", f"--working-directory={path}", "-e", "nvim", "."],
            ),
            ("files", ["xdg-open", str(path)]),
        ):
            calls.clear()
            with patch.object(projects.subprocess, "Popen", popen):
                code = projects.main(
                    ["--home", str(self.home), "open", identifier, action]
                )
            self.assertEqual(code, 0, action)
            argv, kwargs = calls[0]
            self.assertEqual(argv, list(projects.SYSTEMD_RUN_PREFIX) + expected)
            self.assertEqual(argv[argv.index("--") + 1 :], expected)
            self.assertEqual(
                argv[: len(projects.SYSTEMD_RUN_PREFIX)],
                list(projects.SYSTEMD_RUN_PREFIX),
            )
            self.assertIs(kwargs["shell"], False)
            self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
            self.assertIs(kwargs["stdout"], subprocess.DEVNULL)
            self.assertIs(kwargs["stderr"], subprocess.DEVNULL)
            self.assertIs(kwargs["start_new_session"], True)
            self.assertIs(kwargs["close_fds"], True)
            self.assertEqual(kwargs["cwd"], "/")
            self.assertNotIn("gitdir", " ".join(argv))
            self.assertNotIn("/outside/gitdir", " ".join(argv))

    def test_invalid_action_and_unavailable_path_do_not_spawn(self):
        path = self.repo("Repositories/app")
        identifier = projects.project_id(path)
        from io import StringIO

        errors = StringIO()
        with (
            patch.object(projects.sys, "stderr", errors),
            patch.object(
                projects.subprocess,
                "Popen",
                side_effect=AssertionError("launched"),
            ),
        ):
            code = projects.main(
                ["--home", str(self.home), "open", identifier, "terminal;touch"]
            )
        self.assertEqual(code, 1)
        self.assertEqual(errors.getvalue().strip(), "invalid action")
        missing = self.home / "Repositories" / "missing"
        self.assertFalse(projects.launchable(self.home, missing))
        link = self.home / "Repositories" / "link"
        link.symlink_to(path)
        self.assertFalse(projects.launchable(self.home, link))
        with (
            patch.object(projects, "discover_projects", return_value=[missing]),
            patch.object(
                projects.subprocess,
                "Popen",
                side_effect=AssertionError("launched"),
            ),
        ):
            code = projects.open_project(
                self.home, projects.project_id(missing), "editor"
            )
        self.assertEqual(code, 1)

    def test_launch_failure_is_reported_without_a_path(self):
        path = self.repo("Repositories/app")
        from io import StringIO

        errors = StringIO()
        with (
            patch.object(projects.sys, "stderr", errors),
            patch.object(
                projects.subprocess,
                "Popen",
                side_effect=FileNotFoundError("ghostty"),
            ),
        ):
            code = projects.open_project(
                self.home, projects.project_id(path), "terminal"
            )
        self.assertEqual(code, 1)
        self.assertEqual(errors.getvalue().strip(), "unable to launch")
        self.assertNotIn(str(path), errors.getvalue())

    def test_source_does_not_shell_out_during_catalogue(self):
        source = (ROOT / "home/config/launcher/projects.py").read_text()
        self.assertNotIn("shell=True", source)
        self.assertNotIn("os.system", source)
        self.assertNotIn(".envrc", source)
        self.assertNotIn("direnv", source)
        self.assertNotIn("list(os.scandir", source)
