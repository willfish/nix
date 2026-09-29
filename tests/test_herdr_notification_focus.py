import tempfile
import unittest
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = spec_from_file_location(
    "herdr_notification_focus",
    ROOT / "home/config/hyprland/omapager/herdr_notification_focus.py",
)
focus = module_from_spec(SPEC)
SPEC.loader.exec_module(focus)

PATCH_SPEC = spec_from_file_location(
    "patch_herdr_focus",
    ROOT / "home/config/hyprland/omapager/patch-herdr-focus.py",
)
patcher = module_from_spec(PATCH_SPEC)
PATCH_SPEC.loader.exec_module(patcher)


def snapshot():
    return {
        "workspaces": [
            {"workspace_id": "w1", "label": "dot", "number": 1},
            {"workspace_id": "w3", "label": "admin", "number": 3},
        ],
        "tabs": [
            {"tab_id": "w3:t1", "workspace_id": "w3", "label": "1"},
            {
                "tab_id": "w3:t2",
                "workspace_id": "w3",
                "label": "2 hadleigh review",
            },
            {"tab_id": "w1:t1", "workspace_id": "w1", "label": "1"},
        ],
        "agents": [
            {
                "agent": "pi",
                "display_agent": "pi · medium",
                "agent_status": "working",
                "pane_id": "w3:p1",
                "tab_id": "w3:t2",
                "workspace_id": "w3",
                "state_change_seq": 4,
            },
            {
                "agent": "pi",
                "display_agent": "pi · medium",
                "agent_status": "done",
                "pane_id": "w3:p2",
                "tab_id": "w3:t2",
                "workspace_id": "w3",
                "state_change_seq": 9,
            },
            {
                "agent": "pi",
                "agent_status": "idle",
                "pane_id": "w3:p3",
                "tab_id": "w3:t2",
                "workspace_id": "w3",
                "state_change_seq": 3,
            },
        ],
    }


class HerdrNotificationFocusTest(unittest.TestCase):
    def test_parse_real_ghostty_toast_shapes(self):
        parsed = focus.parse_toast(
            "Ghostty", "pi finished: admin · 3 · 2 hadleigh review"
        )
        self.assertEqual(parsed["agent"], "pi")
        self.assertEqual(parsed["event"], "finished")
        self.assertEqual(parsed["workspace_label"], "admin")
        self.assertEqual(parsed["workspace_number"], 3)
        self.assertEqual(parsed["tab_label"], "2 hadleigh review")
        attention = focus.parse_toast(
            "Ghostty", "pi needs attention: admin · 3"
        )
        self.assertEqual(attention["tab_label"], None)
        self.assertIsNone(focus.parse_toast("Slack", "pi finished: admin · 3"))

    def test_long_tab_label_stays_intact(self):
        parsed = focus.parse_toast(
            "Ghostty",
            "pi finished: dot · 1 · 1 Can you fix the race · later",
        )
        self.assertEqual(parsed["tab_label"], "1 Can you fix the race · later")

    def test_resolve_picks_latest_matching_pane_in_tab(self):
        target = focus.resolve_target(
            snapshot(),
            focus.parse_toast(
                "Ghostty", "pi finished: admin · 3 · 2 hadleigh review"
            ),
        )
        self.assertEqual(target["pane_id"], "w3:p2")

    def test_single_tab_toast_focuses_that_tab_when_pane_is_ambiguous(self):
        data = snapshot()
        data["tabs"] = [
            tab for tab in data["tabs"] if tab["workspace_id"] == "w1"
        ]
        data["tabs"][0]["label"] = "only"
        data["agents"] = [
            {
                "agent": "pi",
                "agent_status": "done",
                "pane_id": "w1:p1",
                "tab_id": "w1:t1",
                "workspace_id": "w1",
                "state_change_seq": 1,
            },
            {
                "agent": "pi",
                "agent_status": "done",
                "pane_id": "w1:p2",
                "tab_id": "w1:t1",
                "workspace_id": "w1",
                "state_change_seq": 2,
            },
        ]
        target = focus.resolve_target(
            data, focus.parse_toast("Ghostty", "pi finished: dot · 1")
        )
        self.assertEqual(target["pane_id"], "w1:p2")

    def test_remembered_target_is_reused_after_state_moves_on(self):
        toast = focus.parse_toast(
            "Ghostty", "pi needs attention: admin · 3 · 2 hadleigh review"
        )
        data = snapshot()
        data["agents"][0]["agent_status"] = "blocked"
        data["agents"][0]["state_change_seq"] = 11
        data["agents"].append(
            {
                "agent": "pi",
                "agent_status": "working",
                "pane_id": "w3:p9",
                "tab_id": "w3:t2",
                "workspace_id": "w3",
                "state_change_seq": 2,
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "targets.json"
            stored = focus.remember(
                "Ghostty",
                "pi needs attention: admin · 3 · 2 hadleigh review",
                "12.5",
                {"pane_id": "w3:p9", "socket": "/tmp/herdr.sock"},
                cache,
            )
            self.assertEqual(stored["pane_id"], "w3:p9")
            calls = []

            def call(sock, method, params):
                calls.append((str(sock), method, params))
                if method == "session.snapshot":
                    return {"result": {"snapshot": data}}
                return {"result": {"type": "ok"}}

            outcome = focus.run_open(
                "Ghostty",
                "pi needs attention: admin · 3 · 2 hadleigh review",
                "12.5",
                0,
                [Path("/tmp/other.sock")],
                call,
                cache,
                [
                    {
                        "pid": 7,
                        "class": "com.mitchellh.ghostty",
                        "address": "0xabc",
                        "focusHistoryID": 1,
                        "title": "dot",
                    }
                ],
                lambda pid: 0,
            )
        self.assertEqual(calls[0][1], "session.snapshot")
        self.assertEqual(calls[1][0], "/tmp/herdr.sock")
        self.assertEqual(calls[1][1], "pane.focus")
        self.assertEqual(calls[1][2], {"pane_id": "w3:p9"})
        self.assertEqual(outcome["window"]["address"], "0xabc")
        self.assertIsNotNone(toast)

    def test_window_prefers_socket_ancestor_over_other_ghostty(self):
        clients = [
            {
                "pid": 1,
                "class": "com.mitchellh.ghostty",
                "address": "0x1",
                "focusHistoryID": 0,
                "title": "other",
            },
            {
                "pid": 9,
                "class": "com.mitchellh.ghostty",
                "address": "0x9",
                "focusHistoryID": 4,
                "title": "admin",
            },
            {
                "pid": 3,
                "class": "com.william.cliamp",
                "address": "0x3",
                "focusHistoryID": 0,
                "title": "radio",
            },
        ]
        chosen = focus.choose_window(
            clients, ancestors={4, 9}, workspace_label="admin"
        )
        self.assertEqual(chosen["address"], "0x9")

    def test_missing_workspace_label_does_not_follow_the_number(self):
        target = focus.resolve_target(
            snapshot(),
            focus.parse_toast("Ghostty", "pi finished: frontend · 3"),
        )
        self.assertIsNone(target)

    def test_renamed_tab_stays_in_the_workspace(self):
        target = focus.resolve_target(
            snapshot(),
            focus.parse_toast("Ghostty", "pi finished: admin · 3 · old name"),
        )
        self.assertEqual(target, {"workspace_id": "w3"})

    def test_qml_patch_inserts_click_and_remember_hooks(self):
        text = (
            patcher.HELPER_ANCHOR
            + patcher.REMEMBER_OLD
            + patcher.ACTIVATE_OLD
            + "        somewhere else is still a Slack notification\n"
        )
        patched = patcher.patch_text(
            text, "/nix/store/focus/bin/herdr-notification-focus"
        )
        self.assertIn("rememberHerdrTarget(row)", patched)
        self.assertIn("openHerdrTarget(row)", patched)
        self.assertIn("/nix/store/focus/bin/herdr-notification-focus", patched)
        self.assertEqual(patched.count("Source first, link last"), 1)


if __name__ == "__main__":
    unittest.main()
