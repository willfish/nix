"""Exercise the privileged workflow's policy step with local lock-file fixtures."""

import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/auto-merge-flake-lock.yml"


def github_node(owner, repo):
    return {
        "locked": {
            "type": "github", "owner": owner, "repo": repo,
            "rev": "a" * 40, "narHash": "sha256-" + "A" * 43 + "=",
            "lastModified": 1,
        },
        "original": {"type": "github", "owner": owner, "repo": repo},
    }


class FlakeLockPolicyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="flake-lock-policy-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        gh = self.bin / "gh"
        gh.write_text(textwrap.dedent('''\
            #!/usr/bin/env python3
            import base64, json, os, pathlib, sys
            args = sys.argv[1:]
            directory = pathlib.Path(os.environ["LOCK_FIXTURE_DIR"])
            if args[:2] == ["pr", "view"]:
                if args[3:] == ["--json", "files", "--jq", ".files[].path"]:
                    # A newer live PR head looks safe; the event head may not be.
                    print("flake.lock")
                    sys.exit(0)
                assert args == ["pr", "view", "fixture-pr", "--json", "baseRefOid,headRefOid",
                                "--jq", '.baseRefOid + " " + .headRefOid'], args
                print(os.environ["LOCK_CURRENT_BASE"] + " " + os.environ["LOCK_CURRENT_HEAD"])
                sys.exit(0)
            if args[:2] == ["pr", "merge"]:
                pathlib.Path(os.environ["LOCK_FIXTURE_DIR"], "merge.json").write_text(json.dumps(args))
                sys.exit(0)
            if args == ["api", "--method", "GET", "repos/fixture/repo/compare/base...head"]:
                print((directory / "comparison.json").read_text())
                sys.exit(0)
            if args[:4] == ["api", "--method", "GET", "repos/fixture/repo/actions/runs"]:
                print("999")
                sys.exit(0)
            if args == ["api", "--method", "POST", "repos/fixture/repo/actions/runs/999/approve"]:
                (directory / "approved").touch()
                sys.exit(0)
            assert args[:4] == ["api", "--method", "GET", "repos/fixture/repo/contents/flake.lock"], args
            assert args[4] == "-f" and args[6:] == ["--jq", ".content"], args
            assert args[5] in ("ref=base", "ref=head"), args
            source = pathlib.Path(os.environ["LOCK_FIXTURE_DIR"]) / (args[5][4:] + ".lock")
            print(base64.b64encode(source.read_bytes()).decode())
        '''))
        gh.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update(
            PATH=str(self.bin) + os.pathsep + self.env["PATH"],
            LOCK_FIXTURE_DIR=str(self.directory), GH_REPO="fixture/repo",
            BASE_SHA="base", HEAD_SHA="head", AUTO_MERGE_GITHUB_OWNERS="willfish\n",
            PR_URL="fixture-pr", LOCK_CURRENT_BASE="base", LOCK_CURRENT_HEAD="head",
            PR_NUMBER="123", HEAD_REF="update_flake_lock_action",
        )
        self.comparison = {
            "base_commit": {"sha": "base"}, "merge_base_commit": {"sha": "base"},
            "status": "ahead", "total_commits": 1, "commits": [{"sha": "head"}],
            "files": [{"filename": "flake.lock", "status": "modified"}],
        }
        self.base = {
            "version": 7, "root": "root",
            "nodes": {
                "root": {"inputs": {"app": "app"}},
                "app": {**github_node("willfish", "app"), "inputs": {"dep": "dep"}},
                "dep": github_node("NixOS", "nixpkgs"),
            },
        }
        self.head = copy.deepcopy(self.base)

    def run_step(self, name):
        workflow = WORKFLOW.read_text()
        step = workflow.split(f"      - name: {name}\n", 1)[1]
        step = step.split("\n      - name:", 1)[0]
        command = textwrap.dedent(step.split("        run: |\n", 1)[1])
        return subprocess.run(["bash", "-c", command], cwd=ROOT, env=self.env,
                              capture_output=True, text=True, timeout=10)

    def run_policy(self):
        for name, lock in (("base", self.base), ("head", self.head)):
            (self.directory / (name + ".lock")).write_text(
                lock if isinstance(lock, str) else json.dumps(lock))
        return self.run_step("Verify changed flake inputs are auto-mergeable")

    def run_through_approval(self):
        (self.directory / "comparison.json").write_text(json.dumps(self.comparison))
        for name, lock in (("base", self.base), ("head", self.head)):
            (self.directory / (name + ".lock")).write_text(json.dumps(lock))
        for step in ("Verify PR only updates flake.lock",
                     "Verify changed flake inputs are auto-mergeable",
                     "Approve CI workflow run"):
            result = self.run_step(step)
            if result.returncode:
                return result
        return result

    def test_immutable_file_gate_allows_only_modified_lock_before_approval(self):
        result = self.run_through_approval()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.directory / "approved").exists())

    def test_immutable_file_gate_rejects_event_head_changes_despite_safe_live_pr(self):
        self.comparison["files"].append({"filename": ".github/workflows/ci.yml", "status": "modified"})
        result = self.run_through_approval()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.directory / "approved").exists())

    def test_immutable_file_gate_rejects_added_removed_or_renamed_lock(self):
        for status in ("added", "removed", "renamed"):
            with self.subTest(status=status):
                self.comparison["files"][0]["status"] = status
                result = self.run_through_approval()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((self.directory / "approved").exists())

    def test_immutable_file_gate_rejects_mismatched_or_incomplete_comparison(self):
        baseline = copy.deepcopy(self.comparison)
        invalid = (
            {"base_commit": {"sha": "other"}},
            {"merge_base_commit": {"sha": "older"}},
            {"commits": [{"sha": "other"}]},
            {"commits": []}, {"total_commits": 251},
            {"status": "diverged"}, {"files": []}, {"files": None},
            {"files": [{"filename": "flake.lock", "status": "modified", "previous_filename": "other"}]},
            {"files": baseline["files"] + [{"filename": "other", "status": "modified"}] * 299},
        )
        for changes in invalid:
            with self.subTest(changes=changes):
                self.comparison = baseline | changes
                result = self.run_through_approval()
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((self.directory / "approved").exists())

    def assert_allowed(self):
        result = self.run_policy()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def assert_manual(self):
        result = self.run_policy()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("manual review required", result.stderr.lower())
        return result

    def test_unchanged_lock_is_allowed(self):
        self.assert_allowed()

    def test_allowed_direct_revision_update(self):
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_allowed()

    def test_disallowed_transitive_revision_with_unchanged_parent(self):
        self.head["nodes"]["dep"]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_transitive_edge_redirect_requires_manual_review(self):
        self.base["nodes"]["other"] = github_node("willfish", "other")
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["inputs"]["dep"] = "other"
        self.assert_manual()

    def test_added_node_requires_manual_review(self):
        self.head["nodes"]["unused"] = github_node("willfish", "new")
        self.assert_manual()

    def test_removed_node_requires_manual_review(self):
        self.base["nodes"]["unused"] = github_node("willfish", "old")
        self.assert_manual()

    def test_removed_transitive_edge_requires_manual_review(self):
        self.head["nodes"]["app"]["inputs"] = {}
        self.assert_manual()

    def test_allowed_transitive_revision_update_is_reported(self):
        self.base["nodes"]["dep"] = github_node("willfish", "library")
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["dep"]["locked"].update(
            rev="b" * 40, narHash="sha256-" + "B" * 42 + "A=", lastModified=2)
        self.assertIn("dep", self.assert_allowed().stdout)

    def test_mixed_approved_and_disallowed_updates_are_rejected(self):
        for name in ("app", "dep"):
            self.head["nodes"][name]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_disallowed_direct_update_is_rejected(self):
        self.base["nodes"]["app"] = github_node("NixOS", "app")
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_owner_allowlist_is_exact_and_has_no_default(self):
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        for owners in ("", "willfish-other", "Willfish", " willfish"):
            with self.subTest(owners=owners):
                self.env["AUTO_MERGE_GITHUB_OWNERS"] = owners
                self.assert_manual()

    def test_non_github_protocol_cannot_use_approved_owner(self):
        self.base["nodes"]["app"]["locked"]["type"] = "gitlab"
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_locked_source_changes_require_review(self):
        original_head = copy.deepcopy(self.head)
        for field, value in (("owner", "someone-else"), ("repo", "other"),
                             ("type", "gitlab"), ("host", "other.example"),
                             ("dir", "other"), ("ref", "other")):
            with self.subTest(field=field):
                self.head = copy.deepcopy(original_head)
                self.head["nodes"]["app"]["locked"][field] = value
                self.assert_manual()

    def test_custom_github_host_is_not_in_owner_allowlist(self):
        self.base["nodes"]["app"]["locked"]["host"] = "other.example"
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_source_metadata_and_flake_flag_changes_require_review(self):
        self.head["nodes"]["dep"]["original"]["ref"] = "other-branch"
        self.assert_manual()
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["dep"]["flake"] = False
        self.assert_manual()

    def test_added_edge_requires_review(self):
        self.head["nodes"]["app"]["inputs"]["second"] = "dep"
        self.assert_manual()

    def test_root_edges_and_root_identity_cannot_change(self):
        self.head["nodes"]["root"]["inputs"]["app"] = "dep"
        self.assert_manual()
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["other-root"] = self.head["nodes"].pop("root")
        self.head["root"] = "other-root"
        self.assert_manual()

    def test_root_label_does_not_need_to_be_root(self):
        self.base["nodes"]["n1"] = self.base["nodes"].pop("root")
        self.base["root"] = "n1"
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_allowed()

    def test_valid_nested_follows_are_resolved(self):
        self.base["nodes"]["root"]["inputs"].update(alias=["app", "dep"], second=["alias"])
        self.base["nodes"]["app"]["inputs"]["shared"] = ["second"]
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_allowed()

    def test_follows_redirect_requires_review_even_if_destination_matches(self):
        self.base["nodes"]["root"]["inputs"]["alias"] = "dep"
        self.base["nodes"]["app"]["inputs"]["shared"] = ["alias"]
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["inputs"]["shared"] = ["app", "dep"]
        self.assert_manual()

    def test_unresolvable_follows_cycles_require_review(self):
        for aliases in ({"loop": ["loop"]}, {"a": ["b"], "b": ["a"]},
                        {"a": ["app", "shared"]}):
            with self.subTest(aliases=aliases):
                self.base["nodes"]["root"]["inputs"] = {"app": "app", **aliases}
                self.base["nodes"]["app"]["inputs"]["shared"] = [next(iter(aliases))]
                self.head = copy.deepcopy(self.base)
                self.assert_manual()

    def test_empty_follows_path_can_reference_root(self):
        self.base["nodes"]["app"]["inputs"]["parent"] = []
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["app"]["locked"]["rev"] = "b" * 40
        self.assert_allowed()

    def test_dangling_or_malformed_references_require_review(self):
        for reference in ("missing", ["missing"], ["app", "missing"], [1],
                          None, 1, True, {"follows": "app"}):
            with self.subTest(reference=reference):
                self.base["nodes"]["app"]["inputs"]["dep"] = reference
                self.head = copy.deepcopy(self.base)
                self.assert_manual()

    def test_malformed_lock_structure_requires_review(self):
        original = copy.deepcopy(self.base)
        invalid = [None, [], {}, {**original, "version": 8}, {**original, "version": True},
                   {**original, "root": "missing"}, {**original, "nodes": []},
                   {**original, "extension": 1}]
        for lock in invalid:
            with self.subTest(lock=lock):
                self.base = self.head = lock
                self.assert_manual()

    def test_malformed_node_or_source_requires_review(self):
        original = copy.deepcopy(self.base)
        invalid = [None, {}, {"locked": None}, {"extension": 1},
                   {**github_node("willfish", "app"), "inputs": []},
                   {**github_node("willfish", "app"), "flake": 0},
                   {**github_node("willfish", "app"), "inputs": {"dep": "dep"}, "flake": False}]
        for node in invalid:
            with self.subTest(node=node):
                self.base = copy.deepcopy(original)
                self.base["nodes"]["app"] = node
                self.head = copy.deepcopy(self.base)
                self.assert_manual()

    def test_invalid_or_unsupported_locked_attributes_require_review(self):
        original = copy.deepcopy(self.base)
        for field, value in (("rev", "not-pinned"), ("rev", None),
                             ("narHash", "invalid"), ("narHash", "sha256-???"),
                             ("lastModified", True), ("lastModified", -1),
                             ("type", "future-fetcher"), ("owner", ""),
                             ("dir", True), ("ref", 1), ("repo", "../other")):
            with self.subTest(field=field, value=value):
                self.base = copy.deepcopy(original)
                self.base["nodes"]["app"]["locked"][field] = value
                self.head = copy.deepcopy(self.base)
                self.assert_manual()

    def test_duplicate_json_keys_are_rejected(self):
        self.head = json.dumps(self.head).replace('"version": 7', '"version": 7, "version": 7')
        self.assert_manual()

    def test_invalid_json_is_rejected(self):
        self.head = "not json"
        self.assert_manual()

    def test_current_repository_graph_is_supported(self):
        self.base = json.loads((ROOT / "flake.lock").read_text())
        self.head = copy.deepcopy(self.base)
        self.assert_allowed()
        node = next(node for node in self.head["nodes"].values()
                    if node.get("locked", {}).get("owner") == "willfish")
        node["locked"]["rev"] = "b" * 40
        self.assert_allowed()

    def test_unreachable_disallowed_nodes_cannot_change(self):
        self.base["nodes"]["app"]["inputs"] = {}
        self.head = copy.deepcopy(self.base)
        self.head["nodes"]["dep"]["locked"]["rev"] = "b" * 40
        self.assert_manual()

    def test_workflow_executes_only_base_branch_policy(self):
        workflow = WORKFLOW.read_text()
        checkout = workflow.split("      - name: Check out trusted lock policy\n", 1)[1]
        checkout = checkout.split("\n      - name:", 1)[0]
        self.assertIn("ref: ${{ github.event.pull_request.base.sha }}", checkout)
        self.assertIn("persist-credentials: false", checkout)
        self.assertNotIn("head.sha", checkout)
        self.assertLess(workflow.index("Verify PR only updates flake.lock"),
                        workflow.index("Check out trusted lock policy"))
        self.assertLess(workflow.index("python3 -I scripts/check-flake-lock-update.py"),
                        workflow.index("Approve CI workflow run"))

    def test_merge_command_is_bound_to_verified_head(self):
        result = self.run_step("Enable pull request auto-merge")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        command = json.loads((self.directory / "merge.json").read_text())
        self.assertEqual(command, ["pr", "merge", "fixture-pr", "--auto", "--match-head-commit",
                                   "head", "--rebase", "--delete-branch"])

    def test_stale_base_or_head_stops_before_merge_command(self):
        for field in ("LOCK_CURRENT_BASE", "LOCK_CURRENT_HEAD"):
            with self.subTest(field=field):
                self.env.update(LOCK_CURRENT_BASE="base", LOCK_CURRENT_HEAD="head")
                self.env[field] = "newer-commit"
                result = self.run_step("Enable pull request auto-merge")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("manual review required", result.stderr)
                self.assertFalse((self.directory / "merge.json").exists())


if __name__ == "__main__":
    unittest.main()
