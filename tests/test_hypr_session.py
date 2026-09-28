"""Session selection/confirmation contract. No real session commands can run."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/hyprland/session.sh"
COMMANDS = {
    "lock": ["hyprlock"],
    "display-off": ["hyprctl", "dispatch", "dpms", "off"],
    "display-toggle": ["hyprctl", "dispatch", "dpms", "toggle"],
    "suspend": ["systemctl", "suspend", "--ignore-inhibitors"],
    "logout": ["hyprctl", "dispatch", "exit"],
    "reboot": ["systemctl", "reboot"],
    "poweroff": ["systemctl", "poweroff"],
}

MOCK = r"""
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
call = {"argv": [name, *sys.argv[1:]]}
if name == "fuzzel":
    call["input"] = sys.stdin.read()
with open(os.environ["CALLS"], "a") as log:
    log.write(json.dumps(call) + "\n")
if name == "fuzzel":
    prompt = sys.argv[sys.argv.index("--prompt") + 1]
    kind = "CONFIRM" if prompt.startswith("Confirm ") else "MENU"
    status = int(os.environ.get(kind + "_STATUS", "0"))
    if status == 0:
        print(os.environ.get(kind + "_RESPONSE", ""))
else:
    status = int(os.environ.get("COMMAND_STATUS", "0"))
sys.exit(status)
"""


class SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bash = shutil.which("bash")
        cls.settings = json.loads(
            subprocess.check_output(
                [
                    "nix",
                    "eval",
                    "--impure",
                    "--json",
                    "--expr",
                    f"(import {ROOT}/home/config/hyprland/settings.nix)"
                    ".session",
                ],
                text=True,
            )
        )
        cls.entries = cls.settings["entries"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.log = self.directory / "calls"
        self.menu = self.directory / "menu.tsv"
        self.write_menu(self.entries)
        # Strict PATH: never fall through to installed power tools. Run the
        # source, not the Nix wrapper which prepends the real tools to PATH.
        for name in ("cut", "awk"):
            (self.directory / name).symlink_to(shutil.which(name))
        for name in ("systemctl", "hyprctl", "hyprlock", "fuzzel"):
            command = self.directory / name
            command.write_text(f"#!{sys.executable}\n{MOCK}")
            command.chmod(0o755)
        self.env = {
            "PATH": str(self.directory),
            "CALLS": str(self.log),
            "HYPR_SESSION_MENU_FILE": str(self.menu),
            "HYPR_SESSION_FUZZEL_CONFIG": "/dev/null",
            "HYPR_SESSION_PROMPT": self.settings["prompt"],
            "HYPR_SESSION_CONFIRM_NO": self.settings["confirmNo"],
            "HYPR_SESSION_CONFIRM_YES": self.settings["confirmYes"],
        }

    def write_menu(self, entries):
        self.menu.write_text(
            "".join(
                "\t".join(
                    (
                        entry["label"],
                        entry["action"],
                        str(int(entry["confirm"])),
                        entry["confirmText"],
                    )
                )
                + "\n"
                for entry in entries
            )
        )

    def run_session(self, *args, **env):
        self.log.unlink(missing_ok=True)
        result = subprocess.run(
            [self.bash, "-euo", "pipefail", str(SCRIPT), *args],
            env={**self.env, **env},
            capture_output=True,
            text=True,
            timeout=5,
        )
        calls = (
            [json.loads(line) for line in self.log.read_text().splitlines()]
            if self.log.exists()
            else []
        )
        return result, calls

    def assert_dispatch(self, calls, action):
        self.assertEqual(
            [call["argv"] for call in calls if call["argv"][0] != "fuzzel"],
            [COMMANDS[action]],
        )

    def assert_no_dispatch(self, calls):
        self.assertTrue(
            all(call["argv"][0] == "fuzzel" for call in calls), calls
        )

    def test_direct_cli_actions_remain_immediate(self):
        for action in COMMANDS:
            with self.subTest(action=action):
                result, calls = self.run_session(action)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(calls), 1)
                self.assert_dispatch(calls, action)

    def test_select_all_configured_actions_and_confirmation_policy(self):
        for entry in self.entries:
            with self.subTest(action=entry["action"]):
                yes = f'{self.settings["confirmYes"]} {entry["confirmText"]}'
                result, calls = self.run_session(
                    "select", entry["action"], CONFIRM_RESPONSE=yes
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_dispatch(calls, entry["action"])
                prompts = [
                    call for call in calls if call["argv"][0] == "fuzzel"
                ]
                self.assertEqual(len(prompts), int(entry["confirm"]))
                if prompts:
                    self.assertEqual(
                        prompts[0]["input"].splitlines(),
                        [self.settings["confirmNo"], yes],
                    )
                    self.assertEqual(
                        prompts[0]["argv"][-1],
                        f'Confirm {entry["confirmText"]}? ',
                    )

    def test_existing_menu_all_actions_including_logout_and_poweroff(self):
        for entry in self.entries:
            with self.subTest(action=entry["action"]):
                result, calls = self.run_session(
                    "menu",
                    MENU_RESPONSE=entry["label"],
                    CONFIRM_RESPONSE=(
                        f'{self.settings["confirmYes"]} {entry["confirmText"]}'
                    ),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_dispatch(calls, entry["action"])
                self.assertEqual(
                    calls[0]["input"].splitlines(),
                    [row["label"] for row in self.entries],
                )
                self.assertEqual(len(calls), 2 + int(entry["confirm"]))

    def test_destructive_actions_cancel_safely_on_both_routes(self):
        for entry in self.entries:
            if not entry["confirm"]:
                continue
            for route in (("menu",), ("select", entry["action"])):
                for response, status in (
                    ("No", "0"),
                    ("", "0"),
                    ("Yes", "0"),
                    ("invalid", "0"),
                    ("", "1"),
                ):
                    with self.subTest(
                        action=entry["action"],
                        route=route,
                        response=response,
                        status=status,
                    ):
                        result, calls = self.run_session(
                            *route,
                            MENU_RESPONSE=entry["label"],
                            CONFIRM_RESPONSE=response,
                            CONFIRM_STATUS=status,
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assert_no_dispatch(calls)

    def test_confirmation_labels_can_change_without_changing_action(self):
        self.write_menu(
            [
                {
                    "label": "End session",
                    "action": "logout",
                    "confirm": True,
                    "confirmText": "end this session",
                }
            ]
        )
        result, calls = self.run_session(
            "select",
            "logout",
            HYPR_SESSION_CONFIRM_NO="Cancel",
            HYPR_SESSION_CONFIRM_YES="Proceed:",
            CONFIRM_RESPONSE="Proceed: end this session",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            calls[0]["input"], "Cancel\nProceed: end this session\n"
        )
        self.assert_dispatch(calls, "logout")

    def test_policy_is_read_from_table_not_hardcoded(self):
        self.write_menu(
            [
                {
                    "label": "Suspend",
                    "action": "suspend",
                    "confirm": True,
                    "confirmText": "sleep",
                }
            ]
        )
        result, calls = self.run_session(
            "select", "suspend", CONFIRM_RESPONSE="No"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_dispatch(calls)
        self.assertEqual(len(calls), 1)

    def test_default_menu_empty_escape_and_unknown_choice(self):
        for response, status, expected in (
            ("", "0", 0),
            ("", "1", 0),
            ("not a row", "0", 1),
        ):
            with self.subTest(response=response, status=status):
                result, calls = self.run_session(
                    MENU_RESPONSE=response, MENU_STATUS=status
                )
                self.assertEqual(result.returncode, expected)
                self.assert_no_dispatch(calls)

    def test_invalid_select_arguments_and_injection_are_rejected(self):
        for args, expected in (
            (("select",), 64),
            (("select", "lock", "extra"), 64),
            (("select", ""), 1),
            (("select", "shutdown"), 1),
            (("select", "poweroff; reboot"), 1),
            (("select", "$(systemctl poweroff)"), 1),
            (("unknown",), 64),
        ):
            with self.subTest(args=args):
                result, calls = self.run_session(*args)
                self.assertEqual(result.returncode, expected)
                self.assertEqual(calls, [])

    def test_unavailable_ambiguous_or_missing_table_fails_closed(self):
        poweroff = next(
            entry for entry in self.entries if entry["action"] == "poweroff"
        )
        for entries in ([], [poweroff, poweroff]):
            with self.subTest(entries=entries):
                self.write_menu(entries)
                result, calls = self.run_session("select", "poweroff")
                self.assertEqual(result.returncode, 1)
                self.assertEqual(calls, [])
        self.menu.unlink()
        result, calls = self.run_session("select", "poweroff")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(calls, [])

    def test_invalid_table_policy_or_action_never_dispatches(self):
        for row in (
            "Broken\tpoweroff\tinvalid\tpower off\n",
            "Broken\tsystemctl poweroff\t0\tbroken\n",
        ):
            with self.subTest(row=row):
                self.menu.write_text(row)
                result, calls = self.run_session(
                    "menu", MENU_RESPONSE="Broken"
                )
                self.assertEqual(result.returncode, 1)
                self.assert_no_dispatch(calls)

    def test_command_failure_propagates_and_retry_dispatches_once(self):
        for entry in self.entries:
            with self.subTest(action=entry["action"]):
                yes = f'{self.settings["confirmYes"]} {entry["confirmText"]}'
                for status in ("5", "0"):
                    result, calls = self.run_session(
                        "select",
                        entry["action"],
                        CONFIRM_RESPONSE=yes,
                        COMMAND_STATUS=status,
                    )
                    self.assertEqual(
                        result.returncode, int(status), result.stderr
                    )
                    self.assert_dispatch(calls, entry["action"])

    def test_missing_executable_does_not_fall_through_to_real_systemctl(self):
        (self.directory / "systemctl").unlink()
        result, calls = self.run_session("select", "suspend")
        self.assertEqual(result.returncode, 127)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
