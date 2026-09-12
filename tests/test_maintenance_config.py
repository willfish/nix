("""Offline regression checks for pinned plugin reconciliation """
 """and test policy.""")
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "behavior_gate", ROOT / "scripts/check-behavior.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class BehavioralGateTests(unittest.TestCase):
    def test_only_explicit_live_node_checks_may_skip(self):
        allowed = (
            "ok 1 - live # SKIP Set PI_TEAM_LIVE_TEST=1 inside herdr to opt in"
        )
        unexpected = "ok 2 - runtime # SKIP Set PI_CONTEXT_TEST_BIN"
        self.assertEqual(gate.unexpected_node_skips(allowed), [])
        self.assertEqual(gate.unexpected_node_skips("# skipped 4"), [])
        self.assertEqual(gate.unexpected_node_skips(
            allowed + "\n" + unexpected), [unexpected])
        self.assertEqual(gate.unexpected_node_skips(
            "ok 3 # SKIP"), ["ok 3 # SKIP"])

    def test_rejects_mutable_generation_before_setting_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "immutable"):
                gate.configure(Path(tmp))

    def test_theme_fixtures_use_the_candidate_binary_not_user_launcher(self):
        home = Path("/nix/store/fixture-home-manager-generation")
        with patch.object(Path, "resolve", return_value=home), \
                patch.object(Path, "is_file", return_value=True), \
                patch.dict(os.environ):
            gate.configure(home)
            self.assertEqual(
                os.environ["PI_THEME_TEST_BIN"], str(home / "home-path/bin/pi"))

    def test_editor_secret_leftovers_are_ignored_without_creating_them(self):
        result = subprocess.run(
            ["git", "check-ignore", "secrets/.conform.fixture.env.yaml"],
            cwd=ROOT, capture_output=True)
        self.assertEqual(result.returncode, 0)


class HerdrPluginTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.entry = {"id": "fixture.plugin", "owner": "fixture",
            "repo": "plugin", "revision": "a" * 40}
        self.catalog = self.root / "catalog.json"
        self.catalog.write_text(json.dumps([self.entry]))
        self.state = self.root / "state.json"
        self.calls = self.root / "calls.jsonl"
        self.tool = self.root / "herdr"
        self.tool.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
state = Path(os.environ['FIXTURE_STATE'])
args = sys.argv[1:]
with open(os.environ['FIXTURE_CALLS'], 'a') as out:
    out.write(json.dumps(args) + '\\n')
if args[:2] == ['plugin', 'list']:
    print(state.read_text() if state.exists() else '{"result":{"plugins":[]}}')
elif args[:2] == ['plugin', 'install']:
    if os.environ.get('FAIL_INSTALL') == '1':
        sys.exit(1)
    owner, repo = args[2].split('/')
    revision = args[args.index('--ref') + 1]
    if os.environ.get('WRONG_PROVENANCE') == '1':
        owner = 'other'
    state.write_text(json.dumps({'result': {'plugins': [{
        'plugin_id': 'fixture.plugin', 'source': {'kind': 'github',
        'owner': owner, 'repo': repo, 'resolved_commit': revision}}]}}))
else:
    sys.exit(99)
''')
        self.tool.chmod(0o755)

    def run_sync(self, **extra):
        return subprocess.run(
            ["bash", str(ROOT / "home/config/herdr/install-plugins.sh"),
             str(self.tool), str(self.catalog)],
            capture_output=True, text=True,
            env={**os.environ, "FIXTURE_STATE": str(self.state),
                 "FIXTURE_CALLS": str(self.calls), **extra})

    def test_installs_exact_revision_then_skips_existing_pin(self):
        self.assertEqual(self.run_sync().returncode, 0)
        self.assertEqual(self.run_sync().returncode, 0)
        calls = [json.loads(row) for row in self.calls.read_text().splitlines()]
        installs = [row for row in calls if row[1] == "install"]
        self.assertEqual(
            installs, [["plugin", "install", "fixture/plugin", "--ref",
                        "a" * 40, "--yes"]])

    def test_failed_install_retains_existing_state_and_returns_failure(self):
        self.state.write_text('{"result":{"plugins":[]}}')
        before = self.state.read_bytes()
        self.assertNotEqual(self.run_sync(FAIL_INSTALL="1").returncode, 0)
        self.assertEqual(self.state.read_bytes(), before)

    def test_wrong_post_install_provenance_is_failure(self):
        self.assertNotEqual(self.run_sync(WRONG_PROVENANCE="1").returncode, 0)

    def test_invalid_pin_never_calls_herdr(self):
        self.entry["revision"] = "main"
        self.catalog.write_text(json.dumps([self.entry]))
        self.assertNotEqual(self.run_sync().returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_repository_catalog_has_unique_ids_and_immutable_revisions(self):
        entries = json.loads(
            (ROOT / "home/config/herdr/plugins.json").read_text())
        self.assertEqual(len({row["id"] for row in entries}), len(entries))
        for row in entries:
            self.assertRegex(row["revision"], r"^[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
