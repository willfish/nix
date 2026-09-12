"""Read-only checks and a metadata-only diff before a Darwin deployment."""

import argparse
import difflib
import json
import os
from pathlib import Path
import platform
import pwd
import stat
import subprocess


def command(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=10)


def problems(config, run=command):
    errors = []
    if platform.system() != "Darwin":
        errors.append("Target requires macOS")
    if platform.machine() != config["architecture"]:
        errors.append("Target architecture differs from the running machine")
    if run(["/bin/hostname", "-s"]).stdout.strip().lower() != config["host"]:
        errors.append("Register the correct node identity before deployment")
    try:
        user = pwd.getpwnam(config["user"])
        if user.pw_dir != config["home"]:
            errors.append("Account home differs from the target")
    except KeyError:
        errors.append("Provision the account before deployment")
        return errors
    auto = run(
        [
            "/usr/bin/defaults",
            "read",
            "/Library/Preferences/com.apple.loginwindow",
            "autoLoginUser",
        ]
    )
    if auto.returncode == 0:
        errors.append("Disable automatic graphical login before deployment")
    for label in config["legacyUserJobs"]:
        path = (
            Path(config["home"]) / "Library/LaunchAgents" / (label + ".plist")
        )
        loaded = run(["/bin/launchctl", "print", f"gui/{user.pw_uid}/{label}"])
        if path.is_symlink() or path.exists() or loaded.returncode == 0:
            errors.append("Retire legacy user job: " + label)
    for label in config["legacySystemJobs"]:
        path = Path(config["legacySystemDirectory"]) / (label + ".plist")
        loaded = run(["/bin/launchctl", "print", "system/" + label])
        if path.is_symlink() or path.exists() or loaded.returncode == 0:
            errors.append("Retire legacy system job: " + label)
    for path in config["requiredFiles"]:
        if not Path(path).is_file():
            errors.append("Provision required runtime/model file: " + path)
    for path in config["executables"]:
        if not os.access(path, os.X_OK):
            errors.append("Runtime is not executable: " + path)
    key = Path(config["home"]) / ".ssh/id_ed25519"
    try:
        info = key.stat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_mode & 0o077
            or info.st_uid != user.pw_uid
        ):
            errors.append("Shared SSH key must be private and user-owned")
    except OSError:
        errors.append(
            "Provision the approved shared identity before deployment"
        )
    alias = Path(config["home"]) / ".config/sops-nix/secrets"
    if alias.exists() and not alias.is_symlink():
        errors.append("Inspect the unmanaged secret directory before migration")
    if (
        Path(config["systemSecrets"]).exists()
        and not Path(config["installedConfig"]).is_file()
    ):
        errors.append("Existing system secrets are not owned by this profile")
    return errors


def metadata_diff(config):
    old = Path(config["installedConfig"])
    previous = json.loads(old.read_text()) if old.is_file() else {}
    # The generated contract contains only identities, paths and policy, not
    # application environments or secret contents. Ignore unknown old fields.
    previous = {key: previous[key] for key in config if key in previous}
    return "\n".join(
        difflib.unified_diff(
            json.dumps(previous, indent=2, sort_keys=True).splitlines(),
            json.dumps(config, indent=2, sort_keys=True).splitlines(),
            fromfile="installed server policy",
            tofile="candidate server policy",
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    if not str(args.target.resolve()).startswith("/nix/store/"):
        parser.error("target must be an already-built Nix store path")
    print(metadata_diff(config))
    errors = problems(config)
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
