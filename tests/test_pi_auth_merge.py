"""Pi auth merge seeds Codex OAuth and drops leftover OpenAI API keys."""
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/pi/merge-auth.py"


class PiAuthMergeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.auth = Path(self.tmp.name) / "agent" / "auth.json"
        self.refresh = Path(self.tmp.name) / "PI_OPENAI_CODEX_REFRESH"
        self.account = Path(self.tmp.name) / "PI_OPENAI_CODEX_ACCOUNT_ID"
        self.refresh.write_text("refresh-token\n")
        self.account.write_text("acct-1\n")

    def run_merge(self, extra=None):
        command = [
            "python3",
            str(SCRIPT),
            str(self.auth),
            "--drop",
            "openai",
            "--oauth-provider",
            "openai-codex",
            "--refresh-file",
            str(self.refresh),
            "--account-file",
            str(self.account),
        ]
        if extra:
            command.extend(extra)
        subprocess.run(command, check=True)
        return json.loads(self.auth.read_text())

    def test_seeds_oauth_and_drops_api_key(self):
        self.auth.parent.mkdir(parents=True)
        self.auth.write_text(
            json.dumps(
                {
                    "openai": {"type": "api_key", "key": "sk-test"},
                    "xai": {"type": "oauth", "refresh": "keep"},
                }
            )
            + "\n"
        )
        merged = self.run_merge()
        self.assertNotIn("openai", merged)
        self.assertEqual(merged["xai"]["refresh"], "keep")
        self.assertEqual(merged["openai-codex"]["type"], "oauth")
        self.assertEqual(merged["openai-codex"]["refresh"], "refresh-token")
        self.assertEqual(merged["openai-codex"]["accountId"], "acct-1")
        self.assertEqual(merged["openai-codex"]["access"], "")
        self.assertEqual(merged["openai-codex"]["expires"], 0)
        self.assertEqual(stat.S_IMODE(self.auth.stat().st_mode), 0o600)

    def test_does_not_clobber_existing_oauth(self):
        self.auth.parent.mkdir(parents=True)
        self.auth.write_text(
            json.dumps(
                {
                    "openai-codex": {
                        "type": "oauth",
                        "refresh": "live-refresh",
                        "accountId": "acct-live",
                        "access": "live-access",
                        "expires": 99,
                    }
                }
            )
            + "\n"
        )
        stamp = self.auth.stat().st_mtime_ns
        merged = self.run_merge()
        self.assertEqual(merged["openai-codex"]["refresh"], "live-refresh")
        self.assertEqual(merged["openai-codex"]["access"], "live-access")
        self.assertEqual(self.auth.stat().st_mtime_ns, stamp)

    def test_replaces_codex_api_key_with_oauth(self):
        self.auth.parent.mkdir(parents=True)
        self.auth.write_text(
            json.dumps({"openai-codex": {"type": "api_key", "key": "sk-codex"}})
            + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["openai-codex"]["type"], "oauth")
        self.assertEqual(merged["openai-codex"]["refresh"], "refresh-token")

    def test_missing_secrets_still_drop_api_key(self):
        self.refresh.unlink()
        self.auth.parent.mkdir(parents=True)
        self.auth.write_text(
            json.dumps({"openai": {"type": "api_key", "key": "sk"}}) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged, {})
