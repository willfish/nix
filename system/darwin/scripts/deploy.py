"""Deploy a built Darwin system after preflight, retaining recovery data."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile

NIX_ENV = "/nix/var/nix/profiles/default/bin/nix-env"


def run(args):
    subprocess.run(args, check=True)


def linked(path):
    return str(path.resolve()) if path.is_symlink() else None


def deploy(target, profile, current, state, execute=run, home_profile=None):
    # No profile registration or recovery-directory creation before preflight.
    execute([str(target / "sw/bin/darwin-preflight"), "--target", str(target)])
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(state, 0o700)
    with (state / "deploy.lock").open("a") as lock:
        os.chmod(lock.name, 0o600)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Repeat under lock in case another deployment just finished.
        execute(
            [str(target / "sw/bin/darwin-preflight"), "--target", str(target)]
        )
        previous = linked(profile)
        recovery = Path(tempfile.mkdtemp(prefix="rollout-", dir=state))
        (recovery / "generations.json").write_text(
            json.dumps(
                {
                    "target": str(target),
                    "previousProfile": previous,
                    "previousActive": linked(current),
                    "previousHome": linked(home_profile)
                    if home_profile
                    else None,
                },
                indent=2,
            )
            + "\n"
        )
        os.chmod(recovery / "generations.json", 0o600)
        print("Recovery references: " + str(recovery), flush=True)
        try:
            execute([NIX_ENV, "-p", str(profile), "--set", str(target)])
            execute([str(target / "sw/bin/darwin-rebuild"), "activate"])
            if linked(current) != str(target) or linked(profile) != str(target):
                raise RuntimeError("Active system/profile differs from target")
        except Exception:
            if linked(profile) == str(target):
                if previous:
                    execute([NIX_ENV, "-p", str(profile), "--set", previous])
                else:
                    profile.unlink()
            else:
                print("Profile changed concurrently; refusing to overwrite it.")
            print(
                "Activation failed. Profile recovery attempted; services may "
                "have changed. Follow the saved-reference rollback runbook.",
                flush=True,
            )
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("deployment requires explicit sudo authorization")
    target = args.target.resolve(strict=True)
    if not str(target).startswith("/nix/store/"):
        parser.error("target must be an already-built Nix store path")
    for tool in ["darwin-preflight", "darwin-rebuild"]:
        if not os.access(target / "sw/bin" / tool, os.X_OK):
            parser.error("target lacks required deployment tooling")
    contract = json.loads((target / "etc/dotfiles/server.json").read_text())
    deploy(
        target,
        Path("/nix/var/nix/profiles/system"),
        Path("/run/current-system"),
        Path("/var/db/dotfiles/rollouts"),
        home_profile=Path(contract["homeGenerationProfile"]),
    )


if __name__ == "__main__":
    main()
