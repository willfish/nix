import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class DarwinFileLimitsTest(unittest.TestCase):
    def test_launch_daemon_raises_inherited_open_file_limit(self):
        darwin_module = (ROOT / "home/user/darwin.nix").read_text()

        for argument in (
            "/bin/launchctl",
            "limit",
            "maxfiles",
            "65536",
            "245760",
        ):
            self.assertIn(f"<string>{argument}</string>", darwin_module)
        self.assertIn("<key>RunAtLoad</key>", darwin_module)
        self.assertIn("<true/>", darwin_module)

    def test_home_manager_and_hmswitch_manage_the_launch_daemon(self):
        darwin_module = (ROOT / "home/user/darwin.nix").read_text()
        hmswitch = (ROOT / "home/config/bin/hmswitch").read_text()

        self.assertIn("limit.maxfiles.plist", darwin_module)
        self.assertIn("sync_darwin_file_limits", hmswitch)
        self.assertIn("/Library/LaunchDaemons/limit.maxfiles.plist", hmswitch)
        self.assertIn("ulimit -n 65536", hmswitch)

    def test_hmswitch_waits_for_launchd_bootout_before_bootstrap(self):
        hmswitch = (ROOT / "home/config/bin/hmswitch").read_text()
        tailscale_sync = hmswitch.split("sync_darwin_tailscale_service()", 1)[1]
        tailscale_sync = tailscale_sync.split("\n}\n", 1)[0]

        bootout = tailscale_sync.index('sudo launchctl bootout "$label"')
        wait = tailscale_sync.index('wait_for_launchd_service_removal "$label"')
        bootstrap = tailscale_sync.index('sudo launchctl bootstrap system "$target"')

        self.assertLess(bootout, wait)
        self.assertLess(wait, bootstrap)


if __name__ == "__main__":
    unittest.main()
