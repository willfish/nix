("""Exercise the packaged reconnect script's secret-file lifetime """
 """with fake nmcli.""")
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WifiReconnectCleanupTests(unittest.TestCase):
    def run_fixture(self, interrupt):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            secrets = root / "temporary"
            secrets.mkdir()
            nmcli = root / "nmcli"
            nmcli.write_text('''#!/usr/bin/env bash
case "$*" in
  *DEVICE,TYPE,STATE*) echo 'wlan0:wifi:disconnected' ;;
  *DEVICE,STATE*) echo 'wlan0:disconnected' ;;
  *WIFI*) echo enabled ;;
  *GENERAL.CONNECTION*) echo fixture ;;
  *802-11-wireless-security.psk*) echo fixture-password ;;
  *'connection up'*)
    touch "$FIXTURE_READY"
    if [ "$FIXTURE_WAIT" = 1 ]; then sleep 30; fi
    ;;
esac
''')
            nmcli.chmod(0o755)
            source = (ROOT / "home/user/network.nix").read_text()
            script = source.split(
                "                wifi_device() {", 1)[1].split(
                "                # Catch up", 1)[0]
            script = "wifi_device() {" + script
            script = script.replace(
                "${nmcli}", str(nmcli)).replace("''${", "${")
            child = subprocess.Popen(
                ["bash", "-c", "set -euo pipefail\nlast_attempt_epoch=0\n"
                 "min_interval_sec=0\n" + script + "\ntry_reconnect\n"],
                env={**os.environ, "TMPDIR": str(secrets),
                     "FIXTURE_READY": str(root / "ready"),
                     "FIXTURE_WAIT": "1" if interrupt else "0"},
                start_new_session=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            try:
                if interrupt:
                    deadline = time.monotonic() + 5
                    while (not (root / "ready").exists()
                           and child.poll() is None
                           and time.monotonic() < deadline):
                        time.sleep(0.01)
                    self.assertTrue((root / "ready").exists())
                    self.assertEqual(len(list(secrets.iterdir())), 1)
                    self.assertEqual(
                        next(secrets.iterdir()).stat().st_mode & 0o777, 0o600)
                    os.killpg(child.pid, signal.SIGTERM)
                stdout, stderr = child.communicate(timeout=5)
                self.assertEqual(child.returncode,
                                 143 if interrupt else 0, stderr.decode())
                self.assertEqual(list(secrets.iterdir()), [])
                self.assertNotIn(b"fixture-password", stdout + stderr)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate()

    def test_normal_attempt_removes_secret_file(self):
        self.run_fixture(False)

    def test_interrupted_attempt_removes_secret_file(self):
        self.run_fixture(True)


if __name__ == "__main__":
    unittest.main()
