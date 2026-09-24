"""Fixed daily actions. Never interpret launcher queries as commands."""

import os
from pathlib import Path
import subprocess
import sys
import termios
import tty


def launch(action):
    commands = {
        "workspace": ["fish", "-ic", "mux start dot"],
        "notes": ["fish", "-ic", "today"],
        "agenda": ["daily-workflow", "_agenda"],
        "cleanup": ["daily-workflow", "_cleanup"],
    }
    if action not in commands:
        raise ValueError("Unknown daily action")
    env = os.environ.copy()
    # Attach in the new terminal, even when launched from another pane.
    for key in list(env):
        if key.startswith("HERDR_"):
            env.pop(key)
    env["MUX_BACKEND"] = "herdr"
    env.pop("MUX_HERDR_ATTACH", None)
    subprocess.Popen(
        [
            "systemd-run",
            "--user",
            "--scope",
            "--collect",
            "--quiet",
            "--",
            "ghostty",
            *(
                ["--title=Today", "--window-width=84", "--window-height=24"]
                if action == "agenda"
                else (
                    [
                        "--title=Today's notes",
                        "--window-width=100",
                        "--window-height=36",
                    ]
                    if action == "notes"
                    else []
                )
            ),
            "--working-directory=" + str(Path.home()),
            "-e",
            *commands[action],
        ],
        env=env,
        start_new_session=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def cleanup():
    print(
        "Delete old Nix generations and collect garbage for your user AND the"
        " system."
    )
    print("This removes rollback options. Sudo may request your password.")
    try:
        answer = input("Type DELETE to run gcall, or Enter to cancel: ")
    except (EOFError, KeyboardInterrupt):
        return 0
    if answer != "DELETE":
        print("Cancelled. Nothing deleted.")
        return 0
    return subprocess.run(
        [str(Path.home() / ".bin/gcall")], check=False
    ).returncode


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1 or args[0] not in {
        "workspace",
        "notes",
        "agenda",
        "cleanup",
        "_agenda",
        "_cleanup",
    }:
        print(
            "Usage: daily-workflow workspace|notes|agenda|cleanup",
            file=sys.stderr,
        )
        return 64
    try:
        if args[0] in {"_agenda", "_cleanup"}:
            result = (
                cleanup()
                if args[0] == "_cleanup"
                else subprocess.run(
                    ["daily-agenda", "--refresh"], check=False
                ).returncode
            )
            try:
                if args[0] == "_agenda" and sys.stdin.isatty():
                    fd = sys.stdin.fileno()
                    previous = termios.tcgetattr(fd)
                    try:
                        tty.setraw(fd)
                        os.read(fd, 1)
                    finally:
                        termios.tcsetattr(fd, termios.TCSADRAIN, previous)
                else:
                    input("\nPress Enter to close.")
            except (EOFError, KeyboardInterrupt):
                pass
            return result
        launch(args[0])
        return 0
    except OSError:
        print(
            "Daily action could not start. Check installed commands and user"
            " services.",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
