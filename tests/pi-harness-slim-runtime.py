"""Real-Pi catalogue regressions using synthetic skills and local model
fixtures.

PI_SKILL_TEST_EXTENSION must select the trusted candidate catalogue index.ts.
PI_MCP_TEST_BIN optionally selects the packaged Pi executable (default: pi).
PI_HARNESS_TEST_HOME_FILES must select a built Home Manager generation's
home-files directory for the durable full-capability fixture budget case.
Run with Python's standard library, for example:
    python3 tests/pi-harness-slim-runtime.py -v

Each case creates a temporary HOME/profile and whitelists the child environment.
The only model endpoint is a loopback HTTP fixture with a synthetic key. No user
settings, credentials, real skill bodies, or hosted inference are needed. For
OS-enforced isolation, run the entire Python suite inside a network namespace
with loopback enabled, so Pi and its local fixture share the same namespace.
"""

import importlib.util
import json
import os
import re
import shutil
import sys
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

spec = importlib.util.spec_from_file_location(
    "mcp_fixture", Path(__file__).with_name("pi-mcp-runtime.py")
)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class SkillCatalogRuntimeTest(unittest.TestCase):
    def setUp(self):
        extension = Path(os.environ.get("PI_SKILL_TEST_EXTENSION", ""))
        self.assertTrue(extension.is_file(), "Set PI_SKILL_TEST_EXTENSION")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.profile = self.root / ".pi/agent"
        self.profile.mkdir(parents=True)
        self.skills = {}
        for i in range(12):
            self.add_skill(f"fixture-{i:02}", f"Regression topic {i}")
        self.add_skill("manual-only", "Manual secret capability", manual=True)
        project_skill = self.root / ".pi/skills/untrusted/SKILL.md"
        project_skill.parent.mkdir(parents=True)
        project_skill.write_text(
            "---\nname: untrusted\ndescription: Must not be exposed\n"
            "---\nUNTRUSTED_BODY\n")
        (self.profile /
     "settings.json").write_text(json.dumps({"defaultProjectTrust": "no"}))
        server = ThreadingHTTPServer(("127.0.0.1", 0), fixture.ModelHandler)
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.server = server
        model = {"providers": {"fixture": {
            "baseUrl": f"http://127.0.0.1:{server.server_port}/v1",
            "api": "openai-completions", "apiKey": "synthetic",
            "models": [{"id": "fixture", "name": "Fixture", "reasoning": False,
                        "input": ["text"], "contextWindow": 32768,
                        "maxTokens": 1024,
                        "cost": {"input": 0, "output": 0,
                                 "cacheRead": 0, "cacheWrite": 0}}]
        }}}
        (self.profile / "models.json").write_text(json.dumps(model))
        self.command = [
            os.environ.get("PI_MCP_TEST_BIN", "pi"), "--mode", "rpc",
            "--offline", "--no-session", "--provider", "fixture",
            "--model", "fixture", "--thinking", "off", "--no-extensions",
            "--extension", str(extension.resolve()),
            "--no-prompt-templates", "--no-themes",
            "--extension", str(Path(__file__).resolve().parents[1]
                               / "home/config/pi/extensions/reading-policy.ts")]
        self.env = {
            "PATH": os.environ.get("PATH", os.defpath), "HOME": self.temp.name,
            "PI_CODING_AGENT_DIR": str(self.profile), "PI_OFFLINE": "1",
            "PI_TELEMETRY": "0"}

    def add_skill(self, name, description, manual=False):
        path = self.profile / "skills" / name / "SKILL.md"
        path.parent.mkdir(parents=True)
        path.write_text(f"---\nname: {name}\ndescription: {description}\n"
                        + ("disable-model-invocation: true\n" if manual else "")
                        + f"---\nBODY_ONLY_{name}\n")
        self.skills[name] = str(path)

    def client(self, options=()):
        client = fixture.PiClient(
    self.command +
    list(options),
    self.env,
     self.root)
        self.addCleanup(client.close)
        return client

    def catalog(self, client, **args):
        result = client.prompt(json.dumps(
            {"tool": "skill_catalog", "args": args}), "skill_catalog")
        self.assertFalse(result.get("isError"), result)
        return result["result"]

    def test_deployed_full_capability_fixture_stays_within_static_budget(self):
        home_files = Path(os.environ.get("PI_HARNESS_TEST_HOME_FILES", ""))
        deployed = home_files / ".pi/agent"
        self.assertTrue(
            (deployed / "extensions/skill-catalog/index.ts").is_file(),
            "Set PI_HARNESS_TEST_HOME_FILES to generation/home-files")
        self.assertTrue((home_files / ".agents/skills").is_dir())
        shutil.rmtree(self.profile / "skills")
        (self.root / ".agents").mkdir()
        (self.root / ".agents/skills").symlink_to(home_files / \
         ".agents/skills", target_is_directory=True)
        (self.profile / "extensions").symlink_to(deployed / \
         "extensions", target_is_directory=True)
        shutil.copyfile(deployed / "AGENTS.md", self.profile / "AGENTS.md")
        repo = Path(__file__).resolve().parents[1]
        shutil.copyfile(repo / "AGENTS.md", self.root / "AGENTS.md")
        # Never copy deployed models.json, auth.json, settings.json or MCP
        # commands. The deployed extensions see only these synthetic services.
        config_dir = self.root / ".config/mcp"
        config_dir.mkdir(parents=True)
        config = {"mcpServers": {"fixture": {
            "command": sys.executable,
            "args": [str(Path(__file__).with_name(
                "pi-mcp-runtime.py").resolve()),
                     "--fixture", str(self.root / "mcp-events.jsonl")],
            "lifecycle": "lazy", "protocolVersion": "legacy",
            "directTools": False,
            "requestTimeoutMs": 5000,
        }}, "settings": {"hostConfigDiscovery": "off", "directTools": False,
                         "namespaceTools": False, "scriptMode": False}}
        (config_dir / "mcp.json").write_text(json.dumps(config))
        self.env["XDG_CONFIG_HOME"] = str(self.root / ".config")
        # Use the deployed auto-discovered extension set rather than a tool
        # allowlist, which could hide registration/capability regressions.
        command = iter(self.command)
        self.command = []
        for argument in command:
            if argument == "--extension":
                next(command)
            elif argument != "--no-extensions":
                self.command.append(argument)
        self.server.final_only = True
        client = self.client()
        client.send(
    type="prompt",
    id="budget",
     message="Local static budget fixture.")
        events = client.until(lambda e: e.get("type") == "agent_end")
        self.assertFalse(any(e.get("success") is False for e in events))
        self.assertEqual(len(self.server.requests), 1)
        request = self.server.requests[0]
        names = {tool["function"]["name"] for tool in request["tools"]}
        self.assertEqual(names, {"read", "bash", "edit", "write", "mcp", "todo",
                                "team", "subagent", "skill_catalog"})
        instructions = "".join(
            message["content"] for message in request["messages"]
            if message["role"] in {"system", "developer"})
        self.assertNotIn("<available_skills>", instructions)
        schemas = json.dumps(
    request["tools"],
    ensure_ascii=False,
    separators=(
        ",",
         ":"))
        total = len(instructions) + len(schemas)
        self.assertLessEqual(total, 23563,
                             "Static fixture instructions + tool schemas "
                             "exceed the 60% reduction budget")
        print(f"Static fixture characters: instructions={len(instructions)}, "
              f"schemas={len(schemas)}, total={total}, ceiling=23563")
        client.send(type="get_commands", id="budget-commands")
        commands = client.until(lambda e: e.get(
            "id") == "budget-commands")[-1]["data"]["commands"]
        self.assertGreaterEqual(
            sum(command["source"] == "skill" for command in commands), 41)
        client.finish()

    def test_discovery_pagination_exact_name_and_manual_command_registry(self):
        client = self.client()
        first = self.catalog(client)
        text = json.dumps(first)
        self.assertNotIn("BODY_ONLY_", text)
        self.assertNotIn("manual-only", text)
        self.assertNotIn("untrusted", text)
        # Assert result membership independently of presentation/details layout.
        visible = lambda result: {
    name for name,
     path in self.skills.items() if path in json.dumps(result)}
        self.assertEqual(visible(first), {f"fixture-{i:02}" for i in range(5)})
        self.assertEqual(visible(self.catalog(client, offset=5)), {
                         f"fixture-{i:02}" for i in range(5, 10)})
        self.assertEqual(visible(self.catalog(client, limit=10)), {
                         f"fixture-{i:02}" for i in range(10)})
        exact = self.catalog(client, query="fixture-11")
        self.assertEqual(visible(exact), {"fixture-11"})
        self.assertIn("Regression topic 11", json.dumps(exact))
        self.assertEqual(
    visible(
        self.catalog(
            client,
            query="nonexistent")),
             set())
        self.assertEqual(visible(self.catalog(client, offset=999)), set())
        client.send(type="get_commands", id="commands")
        response = client.until(lambda e: e.get("id") == "commands")[-1]
        commands = response["data"]["commands"]
        names = {c["name"] for c in commands}
        self.assertTrue({f"skill:{name}" for name in self.skills} <= names)
        self.assertNotIn("skill:untrusted", names)
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertNotIn("<available_skills>", instructions)
        self.assertIn("skill_catalog", instructions)
        self.assertIn(
    "Read whole files only when needed for correctness",
     instructions)
        self.assertNotIn("Always read pi .md files completely", instructions)
        client.finish()

    def test_herdr_guidance_composes_without_launching_interactive_panes(self):
        # Exercise the production guidance hook in real Pi, supplying only team
        # availability. This does not claim to cover Herdr sockets or the TUI.
        controls = Path(__file__).resolve(
        ).parents[1] / "home/config/pi/extensions/subagent/controls.ts"
        extension = self.root / "team-guidance.ts"
        extension.write_text(
            f"import {{ registerTeamControls }} from {
    json.dumps(
        str(controls))};\n"
            "import { Type } from 'typebox';\n"
            "import { StringEnum } from '@earendil-works/pi-ai';\n"
            "export default function(pi) { "
            "registerTeamControls(pi, () => ({}), Type, StringEnum); }"
        )
        client = self.client(["--extension", str(extension)])
        self.catalog(client, query="fixture-11")
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertNotIn("<available_skills>", instructions)
        self.assertIn(
    "Roles: scout (code map), architect (design)",
     instructions)
        self.assertIn("Do not invent human approval.", instructions)
        self.assertIn("never restart its chain", instructions)
        client.finish()

    def test_team_preload_uses_unchanged_registry_after_catalogue_replacement(
        self):
        skills_module = Path(__file__).resolve(
        ).parents[1] / "home/config/pi/extensions/subagent/skills.ts"
        preload = self.root / "team-preload.ts"
        preload.write_text(
            f"import {{ withSkills }} from {json.dumps(str(skills_module))};\n"
            "export default function(pi) { "
            "pi.on('before_agent_start', async event => ({"
            "systemPrompt: await withSkills({name:'fixture', "
            "systemPrompt:event.systemPrompt,"
            "skills:['fixture-11']}, [], "
            "event.systemPromptOptions.skills)})); }"
        )
        client = self.client(["--extension", str(preload)])
        self.catalog(client, query="fixture-11")
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertNotIn("<available_skills>", instructions)
        self.assertIn("BODY_ONLY_fixture-11", instructions)
        self.assertIn("Skill file: " + self.skills["fixture-11"], instructions)
        self.assertEqual(instructions.count("BODY_ONLY_fixture-11"), 1)
        client.finish()

    def test_scout_role_catalogue_does_not_gain_mutation_tools(self):
        persona = (
    Path(__file__).resolve().parents[1] /
     "home/config/pi/agents/scout.md").read_text()
        tools = re.search(r"^tools: (.+)$", persona, re.MULTILINE).group(1)
        expected = {"read", "grep", "find", "ls", "bash", "skill_catalog"}
        self.assertEqual({name.strip() for name in tools.split(",")}, expected)
        client = self.client(["--tools", tools.replace(" ", "")])
        self.catalog(client, query="fixture-11")
        request = self.server.requests[0]
        self.assertEqual({tool["function"]["name"]
                         for tool in request["tools"]}, expected)
        self.assertNotIn(
    "<available_skills>",
     request["messages"][0]["content"])
        client.finish()

    def test_manual_only_discovery_is_marked_and_does_not_load_body(self):
        client = self.client()
        result = self.catalog(client, query="manual-only")
        payload = json.loads(result["content"][0]["text"])
        self.assertEqual(len(payload["results"]), 1)
        self.assertEqual(payload["results"][0]["command"], "/skill:manual-only")
        self.assertTrue(payload["results"][0]["manualOnly"])
        self.assertNotIn("BODY_ONLY_manual-only", json.dumps(result))
        client.finish()

    def test_catalogue_rejects_oversized_page_and_preserves_read_truncation(
        self):
        client = self.client()
        oversized = client.prompt(json.dumps(
            {"tool": "skill_catalog", "args": {"limit": 11}}), "skill_catalog")
        self.assertTrue(oversized.get("isError"))
        path = self.root / "long.txt"
        path.write_text("fixture line\n" * 2100 + "END_SENTINEL\n")
        result = client.prompt(json.dumps(
            {"tool": "read", "args": {"path": str(path)}}), "read")
        text = json.dumps(result)
        self.assertNotIn("END_SENTINEL", text)
        self.assertIn("2000", text)
        client.finish()

    def test_explicit_manual_skill_command_still_expands_full_body(self):
        self.server.final_only = True
        client = self.client()
        client.send(
    type="prompt",
    id="manual",
     message="/skill:manual-only explicit-argument")
        events = client.until(lambda e: e.get("type") == "agent_end")
        self.assertFalse(any(e.get("success") is False for e in events))
        users = [m["content"] for m in self.server.requests[0]
            ["messages"] if m["role"] == "user"]
        self.assertIn("BODY_ONLY_manual-only", json.dumps(users))
        self.assertIn("explicit-argument", json.dumps(users))
        client.finish()

    def test_inactive_catalogue_keeps_generated_skill_advertisement(self):
        client = self.client(["--tools", "read"])
        result = client.prompt(json.dumps(
            {"tool": "read", "args": {
                "path": self.skills["fixture-00"]}}), "read")
        self.assertIn("BODY_ONLY_fixture-00", json.dumps(result))
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertIn("<available_skills>", instructions)
        self.assertIn(self.skills["fixture-11"], instructions)
        self.assertNotIn(self.skills["manual-only"], instructions)
        self.assertEqual({t["function"]["name"]
                         for t in self.server.requests[0]["tools"]}, {"read"})
        client.finish()

    def test_duplicate_exact_generated_catalogue_fails_closed(self):
        extension = self.root / "duplicate.ts"
        extension.write_text(
            "import { formatSkillsForPrompt } "
            "from '@earendil-works/pi-coding-agent';\n"
            "export default function(pi) { "
            "pi.on('before_agent_start', event => ({"
            "systemPrompt: formatSkillsForPrompt("
            "event.systemPromptOptions.skills, 'read')"
            " + '\\nUNRELATED_PREFIX\\n' + event.systemPrompt})); }"
        )
        index = self.command.index("--extension")
        self.command[index:index] = ["--extension", str(extension)]
        client = self.client()
        self.catalog(client, query="fixture-11")
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertEqual(instructions.count("<available_skills>"), 2)
        self.assertIn("UNRELATED_PREFIX", instructions)
        client.finish()

    def test_custom_prompt_and_user_xml_are_preserved(self):
        custom = (
            "CUSTOM_SENTINEL\n<available_skills>USER_XML</available_skills>\n"
            "- When working on pi topics, read the docs and examples, "
            "and follow .md cross-references before implementing\n"
            "- Always read pi .md files completely and follow links to "
            "related docs (e.g., tui.md for TUI API details)")
        context = "CONTEXT_SENTINEL: keep user instructions byte-for-byte."
        (self.root / "AGENTS.md").write_text(context)
        client = self.client(
            ["--system-prompt", custom,
             "--append-system-prompt", "APPEND_SENTINEL"])
        self.catalog(client, query="fixture-00")
        instructions = self.server.requests[0]["messages"][0]["content"]
        self.assertIn(custom, instructions)
        self.assertIn("APPEND_SENTINEL", instructions)
        self.assertIn(context, instructions)
        client.finish()


if __name__ == "__main__":
    unittest.main()
