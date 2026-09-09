"""Isolated Gortex trial gate. No real repositories or user configuration.

Run with GORTEX_TEST_BIN=/nix/store/.../bin/gortex. Optional
GORTEX_TEST_REPORT_DIR retains the fixture and JSON-RPC evidence outside repos.
The accepted boundary is the fixed navigation client, not daemon-wide policy.
A separate test characterizes wider-client writes in an unsandboxed fixture.
"""

import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import time
import unittest


POLICY = "nav,+index_health"
ALLOWED = set(
    """
explore smart_context get_editing_context read_file get_symbol_source
get_file_summary get_symbol search_symbols search_text find_files find_usages
find_implementations find_overrides get_callers get_call_chain get_dependencies
get_dependents get_repo_outline graph_stats index_health
tool_profile tools_search
""".split()
)
# Audited against v0.64.1 internal/daemon/mutating.go, plus the documented
# legacy dispatcher exceptions. Test every name, not just tools/list visibility.
BLOCKED = """
apply_code_action batch_edit edit_file edit_symbol fix_all_in_file inline_symbol
move_symbol rename_symbol safe_delete_symbol scaffold write_file export_graph
generate_docs generate_skill generate_wiki edit_memory feedback notebook_save
notebook_used rename_memory save_note store_memory
surface_memories suppress_finding
enrich_churn enrich_releases index_repository reindex_repository delete_scope
save_scope set_active_project track_repository untrack_repository
forget_checkout
set_primary_checkout reconcile_checkouts post_review overlay_merge analyze
change_contract edit refactor remember workspace_admin publish_review overlay
session workflow nav simulate_chain verify_change check_guards get_diagnostics
""".split()


def snapshot(root):
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


class Client:
    def __init__(self, binary, env, cwd, logs, label, policy=POLICY):
        self.events = queue.Queue()
        self.history = []
        self.stderr = (logs / f"{label}.stderr").open("w")
        self.process = subprocess.Popen(
            [
                binary,
                "mcp",
                "--proxy",
                "--tools",
                policy,
                "--tools-mode",
                "hide",
            ],
            cwd=cwd,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            text=True,
        )
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        self.sequence = 0

    def read(self):
        try:
            for line in self.process.stdout:
                self.events.put(json.loads(line))
        except Exception as error:
            self.events.put(error)
        finally:
            self.events.put(EOFError("Gortex proxy exited"))

    def request(self, method, params=None, timeout=30):
        self.sequence += 1
        request = {"jsonrpc": "2.0", "id": self.sequence, "method": method}
        if params is not None:
            request["params"] = params
        self.history.append({"request": request})
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        while True:
            result = self.events.get(
                timeout=max(0.01, deadline - time.monotonic())
            )
            if isinstance(result, Exception):
                raise result
            self.history.append({"response": result})
            if result.get("id") == self.sequence:
                return result

    def initialize(self):
        response = self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "gortex-trial-fixture", "version": "1"},
            },
        )
        self.process.stdin.write(
            json.dumps(
                {
                    "jsonrpc": "2.0",
                    "method": "notifications/initialized",
                }
            )
            + "\n"
        )
        self.process.stdin.flush()
        return response

    def call(self, name, arguments=None):
        return self.request(
            "tools/call", {"name": name, "arguments": arguments or {}}
        )

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        self.reader.join(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.stderr.close()


class GortexRuntime(unittest.TestCase):
    def setUp(self):
        binary = os.environ.get("GORTEX_TEST_BIN", "gortex")
        self.binary = shutil.which(binary)
        self.assertIsNotNone(
            self.binary, "Set GORTEX_TEST_BIN to the pinned Nix binary"
        )
        report = os.environ.get("GORTEX_TEST_REPORT_DIR")
        if report:
            Path(report).mkdir(parents=True, exist_ok=True, mode=0o700)
        self.root = Path(tempfile.mkdtemp(prefix="gortex-gate-", dir=report))
        self.addCleanup(self.cleanup)
        self.clients = []
        self.unit = None
        self.daemon = None
        self.daemon_log = None
        self.keep = bool(report)
        self.env = {
            key: os.environ[key]
            for key in ("PATH", "LANG")
            if key in os.environ
        }
        for key, directory in {
            "HOME": "home",
            "XDG_CONFIG_HOME": "config",
            "XDG_CACHE_HOME": "cache",
            "XDG_DATA_HOME": "data",
            "XDG_STATE_HOME": "state",
            "XDG_RUNTIME_DIR": "run",
            "TMPDIR": "tmp",
        }.items():
            path = self.root / directory
            path.mkdir(mode=0o700)
            self.env[key] = str(path)
        self.env.update(
            {
                "GORTEX_DAEMON_SOCKET": str(self.root / "run/daemon.sock"),
                "GORTEX_DAEMON_PIDFILE": str(self.root / "run/daemon.pid"),
                "GORTEX_DAEMON_LOGFILE": str(self.root / "cache/daemon.log"),
                "GORTEX_AUTOSTART": "0",
                "GORTEX_TELEMETRY": "off",
                "GORTEX_TOOLS": POLICY,
                "GORTEX_TOOLS_MODE": "hide",
                "NO_COLOR": "1",
                "TERM": "dumb",
            }
        )
        # Unix sockets have a small pathname limit. Reports should use a short
        # external path, e.g. /tmp/gortex-evidence, not a deeply nested repo.
        self.assertLess(len(self.env["GORTEX_DAEMON_SOCKET"].encode()), 104)
        self.repos = []
        for name in ("ruby", "terraform"):
            repo = self.root / name
            repo.mkdir(mode=0o700)
            subprocess.run(
                ["git", "init", "-q", str(repo)], check=True, env=self.env
            )
            self.repos.append(repo)
        (self.repos[0] / "greeter.rb").write_text(
            "class TrialGreeter\n  def greet\n    message\n  end\n"
            "\n  def message\n    'fixture hello'\n  end\nend\n"
        )
        (self.repos[1] / "main.tf").write_text(
            'module "trial_module" {\n  source = "./modules/trial"\n}\n'
        )
        module = self.repos[1] / "modules/trial"
        module.mkdir(parents=True)
        (module / "main.tf").write_text(
            'resource "terraform_data" "trial" {\n  input = "fixture"\n}\n'
        )
        for repo in self.repos:
            subprocess.run(
                ["git", "-C", str(repo), "add", "."], check=True, env=self.env
            )
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(repo),
                    "-c",
                    "user.name=Fixture",
                    "-c",
                    "user.email=fixture@example.invalid",
                    "-c",
                    "commit.gpgsign=false",
                    "commit",
                    "-qm",
                    "fixture",
                ],
                check=True,
                env=self.env,
            )
        self.before = [snapshot(repo) for repo in self.repos]
        self.outside = self.root / "outside.txt"
        self.outside.write_text("outside-root-canary\n")
        config_dir = self.root / "config/gortex"
        config_dir.mkdir(mode=0o700)
        self.config_source = self.root / "config-source.json"
        self.config_source.write_text(
            json.dumps(
                {
                    "repos": [
                        {
                            "path": str(repo),
                            "name": repo.name,
                            "workspace": "trial",
                            "project": "trial",
                        }
                        for repo in self.repos
                    ],
                    "embedding": {"enabled": False},
                    "mcp": {"allow_embedded": False},
                    "exclude": [".git/", ".env", ".env.*", "*.tfstate"],
                }
            )
        )
        self.config_source.chmod(0o444)
        self.config = config_dir / "config.yaml"
        self.config.symlink_to(self.config_source)
        self.config_before = self.config_source.read_bytes()
        self.daemon_log = (self.root / "daemon.stderr").open("w")
        command = [
            self.binary,
            "daemon",
            "start",
            "--backend-path",
            str(self.root / "state/store.sqlite"),
            "--tools",
            POLICY,
            "--tools-mode",
            "hide",
        ]
        launch_env = self.env
        if os.environ.get("GORTEX_TEST_SYSTEMD") == "1":
            # Use a report directory under HOME, not PrivateTmp's /tmp.
            self.assertFalse(str(self.root).startswith("/tmp/"))
            self.unit = self.root.name
            writable = " ".join(
                str(self.root / name)
                for name in ("state", "cache", "data", "run", "tmp")
            )
            command = [
                "systemd-run",
                "--user",
                "--quiet",
                "--pipe",
                "--wait",
                "--collect",
                f"--unit={self.unit}",
                f"--working-directory={self.root / 'home'}",
                "-p",
                "ProtectSystem=strict",
                "-p",
                "ProtectHome=read-only",
                "-p",
                "PrivateTmp=true",
                "-p",
                "NoNewPrivileges=true",
                "-p",
                "RestrictAddressFamilies=AF_UNIX",
                "-p",
                f"ReadWritePaths={writable}",
                "-p",
                "UMask=0077",
                shutil.which("env"),
                "-i",
                *[f"{key}={value}" for key, value in self.env.items()],
                *command,
            ]
            launch_env = os.environ.copy()
        self.daemon = subprocess.Popen(
            command,
            env=launch_env,
            cwd=self.root / "home",
            stdout=self.daemon_log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 60
        while not Path(self.env["GORTEX_DAEMON_SOCKET"]).exists():
            self.assertIsNone(
                self.daemon.poll(), (self.root / "daemon.stderr").read_text()
            )
            self.assertLess(
                time.monotonic(), deadline, "daemon startup timed out"
            )
            time.sleep(0.1)

    def cleanup(self):
        for i, client in enumerate(self.clients):
            client.close()
            (self.root / f"client-{i}.json").write_text(
                json.dumps(client.history, indent=2)
            )
        if self.daemon and self.daemon.poll() is None:
            self.stop_daemon()
            try:
                self.daemon.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait(timeout=10)
        if self.daemon_log:
            self.daemon_log.close()
        if self.keep:
            print(f"Evidence: {self.root}")
        else:
            shutil.rmtree(self.root)

    def stop_daemon(self):
        if self.unit:
            subprocess.run(
                ["systemctl", "--user", "stop", self.unit],
                check=True,
                timeout=40,
            )
        else:
            self.daemon.terminate()

    def client(self, repo=0):
        client = Client(
            self.binary,
            self.env,
            self.repos[repo],
            self.root,
            f"proxy-{len(self.clients)}",
        )
        self.clients.append(client)
        self.assertIn("result", client.initialize())
        return client

    def assert_denied(self, response):
        self.assertTrue(
            "error" in response or response.get("result", {}).get("isError"),
            response,
        )
        # Invalid arguments or an uninitialised store do not prove denial.
        self.assertRegex(
            json.dumps(response).lower(),
            r"blocked|not available|not found|unknown tool|not allowed|"
            r"disabled|prohibited|not exposed",
        )

    def test_full_client_policy_characterization(self):
        # Accepted trial limitation: another client can select full access.
        # The deployed service has additional systemd filesystem restrictions;
        # this fixture deliberately characterizes the standalone binary.
        env = dict(self.env, GORTEX_TOOLS="full")
        client = Client(
            self.binary,
            env,
            self.repos[0],
            self.root,
            "full-proxy",
            policy="full",
        )
        self.clients.append(client)
        self.assertIn("result", client.initialize())
        client.request(
            "tools/list"
        )  # Retain actual registered schemas as evidence.
        # The socket is live before repository admission/index warmup finishes.
        deadline = time.monotonic() + 60
        while True:
            symbols = client.call(
                "search_symbols",
                {
                    "query": "TrialGreeter",
                    "repo": "ruby",
                    "assist": "off",
                },
            )
            if "greeter.rb" in json.dumps(symbols):
                break
            self.assertLess(time.monotonic(), deadline, symbols)
            time.sleep(0.1)
        target = self.repos[0] / "escape.rb"
        response = client.call(
            "write_file", {"path": str(target), "content": "# POLICY ESCAPE\n"}
        )
        if self.unit:
            self.assertFalse(target.exists(), response)
            self.assertIn("read-only file system", json.dumps(response))
        else:
            self.assertTrue(target.exists(), response)
            self.assertEqual(target.read_text(), "# POLICY ESCAPE\n")
            self.assertFalse(
                response.get("result", {}).get("isError"), response
            )

    def test_watcher_add_edit_rename_delete_and_branch_switch(self):
        client = self.client()
        deadline = time.monotonic() + 60
        while (
            '"msg":"daemon: watching"'
            not in (self.root / "daemon.stderr").read_text()
        ):
            self.assertLess(time.monotonic(), deadline, "watcher not ready")
            time.sleep(0.1)

        def await_symbol(name, path=None):
            deadline = time.monotonic() + 30
            while True:
                response = client.call(
                    "search_symbols",
                    {
                        "query": name,
                        "repo": "ruby",
                        "assist": "off",
                        "limit": 20,
                    },
                )
                result = response.get("result", {})
                content = result.get("content", [])
                matches = []
                for item in content:
                    # MCP may append prose session guidance after the JSON.
                    if item.get("type") == "text" and item[
                        "text"
                    ].lstrip().startswith("{"):
                        data = json.loads(item["text"])
                        matches.extend(
                            x
                            for x in (data.get("results") or [])
                            if x.get("name") == name
                        )
                valid = not result.get("isError") and "error" not in response
                if valid and (
                    (path is None and not matches)
                    or any(x.get("file_path") == path for x in matches)
                ):
                    return
                self.assertLess(time.monotonic(), deadline, response)
                time.sleep(0.2)

        source = self.repos[0] / "fresh.rb"
        source.write_text("class AddedFixture\nend\n")
        await_symbol("AddedFixture", "ruby/fresh.rb")
        source.write_text("class EditedFixture\nend\n")
        await_symbol("EditedFixture", "ruby/fresh.rb")
        await_symbol("AddedFixture")
        renamed = source.with_name("renamed.rb")
        source.rename(renamed)
        await_symbol("EditedFixture", "ruby/renamed.rb")
        renamed.unlink()
        await_symbol("EditedFixture")
        repo = self.repos[0]

        def git(*args):
            subprocess.run(
                ["git", "-C", str(repo), *args],
                check=True,
                env=self.env,
                capture_output=True,
            )

        git("checkout", "-qb", "fixture-branch")
        source.write_text("class BranchFixture\nend\n")
        git("add", "fresh.rb")
        git(
            "-c",
            "user.name=Fixture",
            "-c",
            "user.email=f@example.invalid",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-qm",
            "branch fixture",
        )
        await_symbol("BranchFixture", "ruby/fresh.rb")
        git("checkout", "--detach", "HEAD~1")
        await_symbol("BranchFixture")

    def test_trial_gate(self):
        client = self.client()
        tools = client.request("tools/list")["result"]["tools"]
        names = {tool["name"] for tool in tools}
        # tools_search is registered only with lazy discovery enabled; hide
        # mode alone keeps it unregistered in this release.
        self.assertEqual(names, ALLOWED - {"tools_search"})
        print(
            f"Tool schemas: {len(json.dumps(tools).encode())} bytes, "
            f"{len(tools)} tools"
        )
        deadline = time.monotonic() + 60
        while True:
            symbols = client.call(
                "search_symbols",
                {
                    "query": "TrialGreeter",
                    "repo": "ruby",
                    "limit": 5,
                    "assist": "off",
                },
            )
            text = json.dumps(symbols)
            if "greeter.rb" in text:
                break
            self.assertLess(time.monotonic(), deadline, text)
            time.sleep(0.5)
        self.assertIn("line", text)
        self.assertNotIn('"isError": true', text)
        print("Known Ruby symbol found with file/line evidence")
        health = client.call("index_health")
        self.assertFalse(health.get("result", {}).get("isError"), health)
        # Exercise all hidden mutation/dispatcher paths even when not listed.
        for name in BLOCKED:
            with self.subTest(tool=name):
                self.assert_denied(
                    client.call(
                        name,
                        {
                            "path": str(self.repos[0] / "greeter.rb"),
                            "content": "MUTATED",
                            "old_string": "fixture hello",
                            "new_string": "MUTATED",
                            "body": "MUTATED",
                            "command": "touch SHOULD_NOT_EXIST",
                            "confirm": True,
                            "to_disk": True,
                            "kind": "sql_rebuild",
                            "ack": True,
                            "operation": "write_file",
                            "arguments": {
                                "path": str(self.repos[0] / "escape.rb"),
                                "content": "MUTATED",
                            },
                        },
                    )
                )
        for args in (
            {},
            {"tool": "write_file"},
            {"preset": "full", "mode": "defer"},
        ):
            client.call("tool_profile", args)
        client.call(
            "tools_search",
            {"query": "write_file edit execute track memory", "limit": 100},
        )
        self.assert_denied(
            client.call(
                "write_file",
                {
                    "path": str(self.repos[0] / "escape.rb"),
                    "content": "MUTATED",
                },
            )
        )
        self.assertEqual(
            {
                t["name"]
                for t in client.request("tools/list")["result"]["tools"]
            },
            names,
        )
        other = self.client(1)
        self.assertIn(
            "greeter.rb",
            json.dumps(
                other.call(
                    "search_symbols",
                    {"query": "TrialGreeter", "repo": "ruby", "assist": "off"},
                )
            ),
        )
        cross_root = client.call(
            "read_file", {"path": str(self.repos[1] / "main.tf")}
        )
        self.assertIn("trial_module", json.dumps(cross_root))
        outside = client.call("read_file", {"path": str(self.outside)})
        self.assertNotIn("outside-root-canary", json.dumps(outside))
        self.assertTrue(
            outside.get("result", {}).get("isError") or "error" in outside,
            outside,
        )
        self.assertTrue(
            self.config.is_symlink(),
            "startup replaced immutable config symlink",
        )
        self.assertEqual(self.config.read_bytes(), self.config_before)
        for repo, before in zip(self.repos, self.before):
            self.assertEqual(
                snapshot(repo),
                before,
                f"repository artifacts or hooks changed: {repo}",
            )
        self.assertEqual(
            list((self.root / "home").iterdir()), [], "state escaped XDG roots"
        )
        self.stop_daemon()
        self.daemon.wait(timeout=30)
        failed = Client(
            self.binary, self.env, self.repos[0], self.root, "stopped-proxy"
        )
        self.clients.append(failed)
        with self.assertRaises((EOFError, BrokenPipeError)):
            failed.initialize()
        failed.process.wait(timeout=10)
        self.assertNotEqual(failed.process.returncode, 0)
        self.assertFalse(Path(self.env["GORTEX_DAEMON_SOCKET"]).exists())


if __name__ == "__main__":
    os.umask(0o077)
    unittest.main(verbosity=2)
