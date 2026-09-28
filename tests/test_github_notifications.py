import unittest
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = spec_from_file_location(
    "github_watch",
    ROOT / "home/config/hyprland/omapager/github_watch.py",
)
github_watch = module_from_spec(SPEC)
SPEC.loader.exec_module(github_watch)


class GithubNotificationTest(unittest.TestCase):
    def test_issue_and_pull_urls_are_pages(self):
        self.assertEqual(
            github_watch.html_url(
                "https://api.github.com/repos/acme/widgets/issues/12"
            ),
            "https://github.com/acme/widgets/issues/12",
        )
        self.assertEqual(
            github_watch.html_url(
                "https://api.github.com/repos/acme/widgets/pulls/4"
            ),
            "https://github.com/acme/widgets/pull/4",
        )
        self.assertEqual(
            github_watch.html_url(
                "https://api.github.com/repos/acme/widgets/issues/12/comments/9"
            ),
            "https://github.com/acme/widgets/issues/12",
        )
        self.assertEqual(
            github_watch.html_url(
                "https://api.github.com/repos/acme/widgets/issues/comments/9"
            ),
            "",
        )

    def test_announcement_carries_an_open_action(self):
        item = {
            "id": "1",
            "repository": {"full_name": "acme/widgets"},
            "reason": "review_requested",
            "subject": {
                "title": "Fix the parser",
                "url": "https://api.github.com/repos/acme/widgets/pulls/4",
            },
        }
        announcements, seen, seeded = github_watch.plan([item], set(), True)
        self.assertTrue(seeded)
        self.assertEqual(seen, {"1"})
        self.assertEqual(
            announcements[0]["url"],
            "https://github.com/acme/widgets/pull/4",
        )
        self.assertIn(
            "https://github.com/acme/widgets/pull/4",
            announcements[0]["body"],
        )
        command = github_watch.notify_command(announcements[0])
        self.assertIn("open=Open", command)
        self.assertIn("github", command)
        self.assertEqual(github_watch.chosen_action("open\n"), "open")
        self.assertNotEqual(github_watch.chosen_action("closed"), "open")


if __name__ == "__main__":
    unittest.main()
