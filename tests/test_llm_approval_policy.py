"""Compose shared approval policy with synthetic private rules."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHARED = (ROOT / "home/config/llm/AGENTS.md").read_text()
HEADING = "### Approval scope"
POLICY = HEADING + "\n\n" + SHARED.split(HEADING + "\n\n")[1].split(
    "\n\n## Verification")[0]
LEGACY = (
    "For new behaviour, require initial design approval. Legacy policy.")
PRIVATE = (
    "# Synthetic private rules\n\n"
    "Use the private work workflow; never expose credentials.\n\n"
    + LEGACY + "\n\n## Verification\n\n"
    "Keep private verification and framework guidance unchanged.\n"
)


class ApprovalPolicyTest(unittest.TestCase):
    def evaluate(self, private):
        with tempfile.TemporaryDirectory(prefix="approval-policy-") as tmp:
            if private is not None:
                (Path(tmp) / "AGENTS.md").write_text(private)
            expression = f"""
              let
                root = builtins.toPath {json.dumps(str(ROOT))};
                flake = builtins.getFlake (toString root);
                lib = flake.inputs.nixpkgs.lib;
                module = import (root + "/home/user/llm-harness.nix") {{
                  inherit lib;
                  pkgs = flake.inputs.nixpkgs.legacyPackages
                    .${{builtins.currentSystem}};
                  config = {{
                    privateConfig.llm = {{
                      root = builtins.toPath {json.dumps(tmp)};
                      catalog = []; capabilities = {{}};
                    }};
                    dotfiles.capabilities = builtins.mapAttrs (_: _: true)
                      (import (root + "/home/user/skill-capabilities.nix"));
                  }};
                }};
              in builtins.mapAttrs (_: entry: entry.text)
                (lib.filterAttrs (name: _: builtins.elem name
                  [ ".agents/AGENTS.md" ".pi/agent/AGENTS.md" ])
                  module.config.home.file)
            """
            return subprocess.run(
                ["nix", "eval", "--impure", "--json", "--expr", expression],
                cwd=ROOT, capture_output=True, text=True, check=False)

    def test_private_guidance_survives_with_one_canonical_approval_policy(self):
        result = self.evaluate(PRIVATE)
        self.assertEqual(result.returncode, 0, result.stderr)
        rules = json.loads(result.stdout)
        self.assertEqual(len(rules), 2)
        for content in rules.values():
            self.assertEqual(content, PRIVATE.replace(LEGACY, POLICY))
            self.assertEqual(content.count(POLICY), 1)
            self.assertNotIn(LEGACY, content)

    def test_absent_private_rules_use_canonical_rules_unchanged(self):
        result = self.evaluate(None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            list(json.loads(result.stdout).values()), [SHARED, SHARED])

    def test_identical_private_copy_does_not_duplicate_policy(self):
        result = self.evaluate(SHARED)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            list(json.loads(result.stdout).values()), [SHARED, SHARED])

    def test_unknown_or_duplicate_private_boundaries_fail_closed(self):
        for private in [
            PRIVATE.replace(LEGACY, "Different private approval format."),
            PRIVATE + "\nFor new behaviour, another approval policy.\n",
            PRIVATE + "\n" + HEADING + "\n",
            PRIVATE + "\nAn approved objective authorizes conflicts.\n",
        ]:
            with self.subTest(private=private):
                result = self.evaluate(private)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "Private AGENTS.md approval boundary changed",
                    result.stderr)

    def test_representative_scenarios_have_explicit_governing_clauses(self):
        # Policy coverage, not a guarantee of model judgment.
        scenarios = {
            "initial design":
                "obtain initial design approval before implementation",
            "approved inspection and tests":
                "routine reversible steps within scope: inspection, tests",
            "disposable repro": "disposable local repros",
            "authorized download":
                "necessary downloads using already-authorized access",
            "phase or restart": "Approval carries across phases, agents, "
                "context restarts and retries",
            "delegatee": "delegatees inherit the scope",
            "agent-created gate":
                "Agent-proposed gates are not user restrictions",
            "changed risk": "material scope/risk changes",
            "unapproved live action":
                "unapproved destructive/shared/live actions",
            "new access or commitment": "access or external commitments",
            "resolvable ambiguity":
                "consequential ambiguity unresolved by source/context",
            "routine orchestration":
                "choose and report the safe reversible option",
            "mandatory gate": "Preserve explicit exclusions and mandatory "
                "AWS/authentication, manual-only and publishing gates",
        }
        for scenario, clause in scenarios.items():
            with self.subTest(scenario=scenario):
                self.assertIn(clause, POLICY)
        self.assertIn("AWS portal login is explicit-request only", SHARED)


if __name__ == "__main__":
    unittest.main()
