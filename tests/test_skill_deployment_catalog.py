("""Validate deployment data and preserve the pre-registry """
 """Nix deployment contract.""")

import hashlib
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LLM = ROOT / "home/config/llm"
loader = importlib.machinery.SourceFileLoader(
    "audit_skills", str(LLM / "scripts/audit-skills"))
spec = importlib.util.spec_from_loader(loader.name, loader)
audit = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = audit
loader.exec_module(audit)


class SkillDeploymentCatalogTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.catalog = self.root / "catalog.json"
        (self.root / "skills/example").mkdir(parents=True)
        (self.root / "skills/example/SKILL.md").write_text("fixture")
        (self.root / "guides").mkdir()
        (self.root / "guides/example.md").write_text("reference")
        self.entry = {
            "name": "example",
            "kind": "shared",
            "invocation": "reference-backed domain skill",
            "hermes": False,
            "references": {"example.md": "guides/example.md"},
        }

    def load(self, value):
        self.catalog.write_text(json.dumps(value))
        return audit.load_catalog(self.catalog, self.root)

    def test_valid_catalog(self):
        self.assertEqual(self.load([self.entry]), [self.entry])

    def test_invalid_schema(self):
        for value in ({}, [], None, [None], ["example"]):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.load(value)
        for key, value in (
            ("name", "../example"), ("name", "Example"), ("name", "a"),
            ("name", "a" * 65), ("name", "two--hyphens"), ("name", None),
            ("kind", "unknown"), ("kind", []),
            ("invocation", "automatic"), ("invocation", {}),
            ("hermes", "false"), ("hermes", 1),
            ("references", []),
        ):
            entry = dict(self.entry, **{key: value})
            with (self.subTest(key=key, value=value),
                  self.assertRaises(ValueError)):
                self.load([entry])
        for key in self.entry:
            entry = dict(self.entry)
            del entry[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                self.load([entry])
        with self.assertRaises(ValueError):
            self.load([dict(self.entry, unexpected=True)])

    def test_process_cannot_deploy_to_hermes_or_map_references(self):
        (self.root / "process-skills/example").mkdir(parents=True)
        (self.root / "process-skills/example/SKILL.md").write_text("fixture")
        entry = dict(self.entry, kind="process", references={})
        self.assertEqual(self.load([entry]), [entry])
        for changes in ({"hermes": True},
                        {"references": self.entry["references"]}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.load([dict(entry, **changes)])

    def test_duplicate_skills_and_json_keys(self):
        with self.assertRaisesRegex(ValueError, "duplicate skill"):
            self.load([self.entry, self.entry])
        for text in (
            '[{"name":"example","name":"other"}]',
            '[{"references":{"same.md":"a","same.md":"b"}}]',
        ):
            self.catalog.write_text(text)
            with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                audit.load_catalog(self.catalog, self.root)

    def test_reference_source_path_validation(self):
        for source in (
            "../outside.md", "/etc/passwd", "guides/../guides/example.md",
            "guides//example.md", "./guides/example.md", "guides\\example.md",
            "guides/missing.md", "guides", "", None,
        ):
            entry = dict(self.entry, references={"example.md": source})
            with self.subTest(source=source), self.assertRaises(ValueError):
                self.load([entry])

    def test_reference_target_path_validation(self):
        for target in ("../escape.md", "/escape.md", "nested/escape.md",
                       "bad\\name.md", "script.sh", ".md"):
            entry = dict(self.entry, references={target: "guides/example.md"})
            with self.subTest(target=target), self.assertRaises(ValueError):
                self.load([entry])

    def test_symlink_escape(self):
        with tempfile.TemporaryDirectory() as outside:
            secret = Path(outside) / "outside.md"
            secret.write_text("synthetic")
            (self.root / "guides/escape.md").symlink_to(secret)
            with self.assertRaisesRegex(ValueError, "escapes source root"):
                self.load(
                    [dict(self.entry,
                          references={"example.md": "guides/escape.md"})])

    def test_local_reference_collision(self):
        refs = self.root / "skills/example/references"
        refs.mkdir()
        (refs / "example.md").write_text("local")
        with self.assertRaisesRegex(ValueError, "duplicates local reference"):
            self.load([self.entry])

    def test_missing_skill(self):
        (self.root / "skills/example/SKILL.md").unlink()
        with self.assertRaisesRegex(ValueError, "missing catalogue source"):
            self.load([self.entry])

    def test_source_tree_parity(self):
        entries = audit.load_catalog()
        for kind, directory in (("shared", "skills"),
                                ("process", "process-skills")):
            self.assertEqual(
                {e["name"] for e in entries if e["kind"] == kind},
                {p.parent.name for p in (LLM / directory).glob("*/SKILL.md")},
            )
        self.assertEqual(len(entries), 27)
        self.assertEqual(sum(len(e["references"]) for e in entries), 29)
        self.assertEqual({e["name"] for e in entries if e["hermes"]}, set())

    def test_manual_only_gates_and_alias(self):
        entries = audit.load_catalog()
        manual = {e["name"] for e in entries if e["invocation"]
            == "user-invoked router/orchestrator"}
        self.assertEqual(
            manual, {"improve", "skill-evaluation", "skill-router", "teach"})
        for entry in entries:
            base = "skills" if entry["kind"] == "shared" else "process-skills"
            path = LLM / base / entry["name"] / "SKILL.md"
            self.assertEqual(audit.frontmatter_bool(
                path, "disable-model-invocation"), entry["name"] in manual)
            if entry["name"] in manual:
                metadata = (path.parent / "agents/openai.yaml").read_text()
                self.assertRegex(
                    metadata, r"(?m)^\s+allow_implicit_invocation: false\s*$")
        by_name = {e["name"]: e for e in entries}
        self.assertIn("skill-router", by_name)

    def test_audit(self):
        result = subprocess.run(
            [sys.executable, str(LLM / "scripts/audit-skills")],
            cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("No findings.", result.stdout)

    @unittest.skipUnless(
        shutil.which("nix"), "Nix is required for deployment evaluation")
    def test_nix_deployment_matches_overlay_baseline(self):
        # Intercept runCommand, not the deployment logic: capture every
        # generated copy command plus direct home.file sources without
        # building or activating.
        expression = '''
          let
            f = builtins.getFlake (toString ./.);
            lib = f.inputs.nixpkgs.lib;
            m = import ./home/user/llm-harness.nix {
              inherit lib;
              config = f.homeConfigurations.william-linux.config;
              pkgs.runCommand = name: attrs: script: { inherit name script; };
            };
          in m.config.home.file
        '''
        result = subprocess.run(
            ["nix", "eval", "--impure", "--json", "--expr", expression],
            cwd=ROOT, text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        # Store hashes include unrelated config contents. Retain all relative
        # sources, targets, command order, modes, recursion and force flags.
        normalized = re.sub(
            r"/nix/store/[a-z0-9]{32}-", "/nix/store/<hash>-", result.stdout)
        deployment = json.loads(normalized)
        self.assertEqual(len(deployment), 52)
        self.assertEqual(sum(isinstance(v["source"], dict)
                         for v in deployment.values()), 48)
        digest = hashlib.sha256(json.dumps(
            deployment, sort_keys=True).encode()).hexdigest()
        # Reviewed merged-overlay deployment, independent of store hashes.
        # Intentional deployment changes require reviewing this fingerprint.
        self.assertEqual(
            digest,
            "1305bd9e013db8a2f0a652b1d3464b26060bea627a329175be67c35d8851c423")


if __name__ == "__main__":
    unittest.main()
