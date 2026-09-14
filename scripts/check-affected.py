#!/usr/bin/env python3
"""Check only configurations affected by the revisions being pushed."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

LINUX_HOSTS = {"andromeda", "foundation", "starfish", "terminus"}
WORKSTATIONS = LINUX_HOSTS - {"terminus"}
LINUX = "x86_64-linux"
DARWIN = "aarch64-darwin"
MARKER = "dotfiles-pre-push"


@dataclass
class Selection:
    systems: set[str] = field(default_factory=set)
    homes: set[str] = field(default_factory=set)
    checks: set[str] = field(default_factory=set)

    def all_homes(self) -> None:
        # nix-darwin consumes the selected home configuration in specialArgs.
        self.systems.add("relay")
        self.homes.update({f"william@{host}" for host in LINUX_HOSTS})
        self.homes.update({"william-linux", "william@relay"})
        self.checks.update(
            {"host-capabilities", "home-profiles", "headless-darwin"}
        )

    def all_targets(self) -> None:
        self.checks.add("affected-push")
        self.systems.update(LINUX_HOSTS | {"relay"})
        self.all_homes()


def select(paths: list[str]) -> Selection:
    result = Selection()
    for path in paths:
        if path.startswith(("docs/", "plans/", ".github/")) or path in {
            "README.md",
            "AGENTS.md",
            ".gitignore",
        }:
            continue
        result.checks.add("pre-commit")
        parts = path.split("/")
        if len(parts) > 2 and parts[0] == "system" and parts[1] in LINUX_HOSTS:
            result.systems.add(parts[1])
        elif path.startswith("system/darwin/"):
            result.systems.add("relay")
            result.homes.add("william@relay")
            result.checks.add("headless-darwin")
        elif path == "system/modules/workstation.nix":
            result.systems.update(WORKSTATIONS)
        elif path == "system/modules/server.nix":
            result.systems.add("terminus")
        elif path.startswith("system/modules/"):
            result.systems.update(LINUX_HOSTS)
        elif path.startswith("home/"):
            result.all_homes()
        else:
            # Flake/input changes and unmapped paths fail closed. Extend this
            # policy alongside new hosts or dependency boundaries.
            result.all_targets()
    return result


def targets(selection: Selection, native: str) -> tuple[list[str], list[str]]:
    evaluate = []
    build = []
    for host in sorted(selection.systems):
        platform = DARWIN if host == "relay" else LINUX
        attr = (
            f"darwinConfigurations.{host}.system"
            if host == "relay"
            else f"nixosConfigurations.{host}.config.system.build.toplevel"
        )
        evaluate.append(attr)
        if platform == native:
            build.append(attr)
    for home in sorted(selection.homes):
        platform = DARWIN if home == "william@relay" else LINUX
        attr = f'homeConfigurations."{home}".activationPackage'
        evaluate.append(attr)
        if platform == native:
            build.append(attr)
    checks = set(selection.checks)
    if native == DARWIN and "william@relay" in selection.homes:
        checks.add("headless-browser")
    build.extend(f"checks.{native}.{name}" for name in sorted(checks))
    return evaluate, build


def git(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).rstrip(
        "\n"
    )


def changed_paths(old: str | None, new: str) -> list[str]:
    command = (
        ["diff", "--name-only", "--no-renames", "-z", old, new, "--"]
        if old
        else ["ls-tree", "-r", "--name-only", "-z", new]
    )
    return [p for p in git(*command).split("\0") if p]


def remote_commits(remote: str) -> list[str]:
    # Query the remote, not stale tracking refs. Missing local objects merely
    # widen the check; never fetch or rewrite the user's refs from a hook.
    tips = set()
    for line in git("ls-remote", "--refs", remote).splitlines():
        oid, _ = line.split("\t", 1)
        commit = subprocess.run(
            ["git", "rev-parse", "--verify", f"{oid}^{{commit}}"],
            text=True,
            capture_output=True,
        )
        if commit.returncode == 0:
            tips.add(commit.stdout.strip())
    return sorted(tips)


def revisions(lines: list[str], remote: str) -> dict[str, list[str]]:
    selected: dict[str, set[str]] = {}
    tips = None
    for line in lines:
        _, local_oid, _, remote_oid = line.split()
        if set(local_oid) == {"0"}:
            continue  # Branch/tag deletion has no candidate to check.
        new = git("rev-parse", "--verify", f"{local_oid}^{{commit}}")
        if set(remote_oid) != {"0"}:
            old = git("rev-parse", "--verify", f"{remote_oid}^{{commit}}")
            paths = changed_paths(old, new)
        else:
            if tips is None:
                tips = remote_commits(remote)
            commits = git("rev-list", new, "--not", *tips).splitlines()
            paths = []
            for commit in commits:
                # Include both sides of renames and merge changes. Compare
                # each new commit to its first parent, including root commits.
                parents = git(
                    "rev-list", "--parents", "-n", "1", commit
                ).split()
                paths.extend(
                    changed_paths(
                        parents[1] if len(parents) > 1 else None, commit
                    )
                )
        selected.setdefault(new, set()).update(paths)
    return {rev: sorted(paths) for rev, paths in selected.items()}


def run_checks(root: Path, selection: Selection, native: str) -> None:
    evaluate, build = targets(selection, native)
    # Git sources exclude ignored files, secrets and build outputs in manual
    # mode. Archived push snapshots contain only committed files.
    flake = f"git+file:{root}" if (root / ".git").exists() else f"path:{root}"
    for attr in evaluate:
        mode = (
            "build"
            if attr in build
            else "evaluation only (non-native platform)"
        )
        print(f"pre-push: {attr}: {mode}", flush=True)
        subprocess.run(
            [
                "nix",
                "eval",
                "--no-write-lock-file",
                "--raw",
                f"{flake}#{attr}.drvPath",
            ],
            check=True,
        )
        print(flush=True)
    if build:
        print("pre-push: building " + ", ".join(build), flush=True)
        subprocess.run(
            [
                "nix",
                "build",
                "--no-write-lock-file",
                "--no-link",
                "-L",
                *[f"{flake}#{attr}" for attr in build],
            ],
            check=True,
        )
    else:
        print("pre-push: no affected configuration checks", flush=True)


def check_revision(revision: str, selection: Selection, native: str) -> None:
    if not selection.checks and not selection.systems and not selection.homes:
        print(
            f"pre-push: {revision[:12]}: no affected configuration checks",
            flush=True,
        )
        return
    # A clean snapshot prevents dirty files, another checked-out branch, or
    # formatter hooks from changing what is validated or touching the worktree.
    with tempfile.TemporaryDirectory(prefix="dotfiles-pre-push-") as temp:
        root = Path(temp)
        archive = root / "source.tar"
        subprocess.run(
            ["git", "archive", "--format=tar", f"--output={archive}", revision],
            check=True,
        )
        with tarfile.open(archive) as source:
            source.extractall(root, filter="data")
        archive.unlink()
        run_checks(root, selection, native)


def install(executable: Path) -> None:
    hooks = Path(git("rev-parse", "--git-path", "hooks"))
    hooks.mkdir(parents=True, exist_ok=True)
    hook = hooks / "pre-push"
    if hook.exists() or hook.is_symlink():
        if not hook.is_symlink() or Path(os.readlink(hook)).name != MARKER:
            raise ValueError(f"Refusing to replace unmanaged hook: {hook}")
        if os.readlink(hook) == str(executable):
            return
        hook.unlink()
    hook.symlink_to(executable)
    print(f"Installed {hook}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("remote", nargs="?", default="origin")
    parser.add_argument("url", nargs="?")
    parser.add_argument("--install", type=Path)
    parser.add_argument(
        "--base", help="Check the current worktree against this revision"
    )
    parser.add_argument(
        "--plan", action="store_true", help="Print selection without Nix checks"
    )
    args = parser.parse_args()
    try:
        if args.install:
            install(args.install)
            return 0
        os.chdir(git("rev-parse", "--show-toplevel"))
        if args.base:
            paths = [
                p
                for p in git(
                    "diff", "--name-only", "--no-renames", "-z", args.base, "--"
                ).split("\0")
                if p
            ]
            untracked = [
                p
                for p in git(
                    "ls-files", "--others", "--exclude-standard", "-z"
                ).split("\0")
                if p
            ]
            if untracked and not args.plan:
                raise ValueError(
                    "Stage intended new files before checking the worktree"
                )
            paths += untracked
            candidates = {"worktree": paths}
        else:
            candidates = revisions(
                sys.stdin.read().splitlines(), args.url or args.remote
            )
        native = None
        for revision, paths in candidates.items():
            selection = select(paths)
            print(
                f"pre-push: {revision}: systems={sorted(selection.systems)}, "
                f"homes={sorted(selection.homes)}, "
                f"checks={sorted(selection.checks)}",
                flush=True,
            )
            if args.plan or not (
                selection.systems or selection.homes or selection.checks
            ):
                continue
            if native is None:
                native = subprocess.check_output(
                    [
                        "nix",
                        "eval",
                        "--impure",
                        "--raw",
                        "--expr",
                        "builtins.currentSystem",
                    ],
                    text=True,
                ).strip()
            if revision == "worktree":
                run_checks(Path.cwd(), selection, native)
            else:
                check_revision(revision, selection, native)
        return 0
    except (
        subprocess.CalledProcessError,
        ValueError,
        OSError,
        tarfile.TarError,
    ) as error:
        print(f"pre-push: blocked: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
