import base64
import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "home/config/local-llm")
)

spec = importlib.util.spec_from_file_location(
    "declaration",
    Path(__file__).resolve().parents[1]
    / "home/config/local-llm/hermes_declaration.py",
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class DeclarationTests(unittest.TestCase):
    def declaration(self):
        return {
            "version": 1,
            "files": [
                {
                    "path": "config.yaml",
                    "content": base64.b64encode(b"model: test\n").decode(),
                }
            ],
            "jobs": [],
        }

    def test_fresh_home_and_idempotence_preserve_history(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "sessions").mkdir()
            history = root / "sessions/keep"
            history.write_text("history")
            m.apply(root, self.declaration())
            before = (root / "config.yaml").stat().st_mtime_ns
            m.apply(root, self.declaration())
            self.assertEqual(before, (root / "config.yaml").stat().st_mtime_ns)
            self.assertEqual(history.read_text(), "history")
            self.assertEqual(
                (root / "config.yaml").stat().st_mode & 0o777, 0o600
            )

    def test_drift_and_rollback_are_backed_up(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "config.yaml").write_text("prior")
            m.apply(root, self.declaration())
            self.assertEqual(
                [
                    p.read_text()
                    for p in (root / "backups/home-manager").iterdir()
                ],
                ["prior"],
            )
            declaration = self.declaration()
            declaration["files"][0]["content"] = base64.b64encode(
                b"prior"
            ).decode()
            m.apply(root, declaration)
            self.assertEqual((root / "config.yaml").read_text(), "prior")

    def test_reject_unsafe_paths_and_duplicates(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "linked").symlink_to(root, target_is_directory=True)
            for path in ["../outside", "/tmp/outside", "linked/config"]:
                declaration = self.declaration()
                declaration["files"][0]["path"] = path
                with self.assertRaises(ValueError):
                    m.apply(root, declaration)
            declaration = self.declaration()
            declaration["files"] *= 2
            with self.assertRaises(ValueError):
                m.apply(root, declaration)

    def test_qwen_overlay_and_key_are_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            declaration = self.declaration()
            declaration["files"].append(
                {
                    "path": "profiles/qwen/config.yaml",
                    "content": base64.b64encode(
                        b"model:\n  api_key: fixture-key\n"
                    ).decode(),
                }
            )
            overlay = {"model": {"default": "new-model"}}
            key_file = root / "key"
            m.apply(root, declaration, overlay, key_file)
            profile = root / "profiles/qwen/config.yaml"
            before = profile.stat().st_mtime_ns
            m.apply(root, declaration, overlay, key_file)
            self.assertEqual(before, profile.stat().st_mtime_ns)
            self.assertEqual(key_file.read_text().strip(), "fixture-key")
            self.assertIn("new-model", profile.read_text())

    def test_seed_credentials_and_stale_files(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            declaration = self.declaration()
            declaration["seed_files"] = [
                {
                    "path": "auth.json",
                    "content": base64.b64encode(b"initial").decode(),
                }
            ]
            declaration["files"].append(
                {
                    "path": "skills/old.md",
                    "content": base64.b64encode(b"old").decode(),
                }
            )
            m.apply(root, declaration)
            (root / "auth.json").write_text("refreshed")
            declaration["files"].pop()
            m.apply(root, declaration)
            self.assertEqual((root / "auth.json").read_text(), "refreshed")
            self.assertFalse((root / "skills/old.md").exists())

    def test_export_separates_configuration_and_state(self):
        from hermes_export import capture
        import json

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in [
                "config.yaml",
                "SOUL.md",
                "AGENTS.md",
                "profiles/qwen/config.yaml",
                "auth.json",
                "skills/example/SKILL.md",
                "sessions/private.json",
            ]:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
            (root / "cron").mkdir()
            (root / "cron/jobs.json").write_text(
                json.dumps(
                    {
                        "jobs": [
                            {
                                "id": "fixture",
                                "last_run_at": "yesterday",
                                "schedule": {"kind": "cron"},
                                "repeat": {"times": None, "completed": 4},
                            }
                        ]
                    }
                )
            )
            declaration = capture(root)
            paths = {item["path"] for item in declaration["files"]}
            self.assertIn("skills/example/SKILL.md", paths)
            self.assertNotIn("auth.json", paths)
            self.assertNotIn("sessions/private.json", paths)
            self.assertEqual(declaration["seed_files"][0]["path"], "auth.json")
            self.assertNotIn("last_run_at", declaration["jobs"][0])
            self.assertEqual(declaration["jobs"][0]["repeat"]["completed"], 0)

    def test_job_history_and_completed_one_shots_survive(self):
        spec = {
            "id": "one",
            "schedule": {"kind": "once"},
            "enabled": True,
            "repeat": {"times": 1, "completed": 0},
        }
        old = {
            **spec,
            "state": "completed",
            "last_run_at": "yesterday",
            "repeat": {"times": 1, "completed": 1},
        }
        got = m.reconcile_jobs([spec], [old])[0]
        self.assertEqual(got["repeat"]["completed"], 1)
        self.assertFalse(got["enabled"])
        self.assertEqual(got["last_run_at"], "yesterday")

    def test_job_definitions_and_history(self):
        spec = {
            "id": "one",
            "schedule": {"kind": "cron"},
            "prompt": "declared",
        }
        old = {
            **spec,
            "prompt": "drift",
            "next_run_at": "later",
            "last_error": None,
        }
        got = m.reconcile_jobs([spec], [old, {"id": "extra"}])
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["prompt"], "declared")
        self.assertEqual(got[0]["next_run_at"], "later")
        changed = {**spec, "schedule": {"kind": "interval"}}
        self.assertNotIn("next_run_at", m.reconcile_jobs([changed], [old])[0])


if __name__ == "__main__":
    unittest.main()
