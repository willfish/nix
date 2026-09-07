"""Offline regression checks against a built, immutable Home Manager generation.

Run with MCP_TEST_HOME set to the activation package output. All subprocesses
use isolated homes and synthetic credentials; no deployed wrapper is executed
against the user's credentials or an external service.
"""

import json
import os
from pathlib import Path
import queue
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SLACK_TOOLS = {
    "conversations_history",
    "conversations_replies",
    "conversations_add_message",
    "reactions_add",
    "conversations_search_messages",
    "channels_list",
    "users_search",
}
SLACK_ENV_KEYS = (
    "SLACK_MCP_XOXC_TOKEN",
    "SLACK_MCP_XOXD_TOKEN",
    "SLACK_MCP_ADD_MESSAGE_TOOL",
    "SLACK_MCP_REACTION_TOOL",
    "SLACK_MCP_XOXP_TOKEN",
    "SLACK_MCP_XOXB_TOKEN",
)


def isolated_env(directory, **overrides):
    return {
        "PATH": os.environ.get("PATH", os.defpath),
        "HOME": str(directory),
        "XDG_CONFIG_HOME": str(directory / "config"),
        "XDG_CACHE_HOME": str(directory / "cache"),
        **overrides,
    }


class StdioClient:
    """Bounded JSON-RPC client which rejects diagnostics on protocol stdout."""

    def __init__(self, command, env, directory):
        self.messages = queue.Queue()
        self.stdout_errors = []
        self.next_id = 0
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen(
            command,
            env=env,
            cwd=directory,
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
        )
        self.reader = threading.Thread(target=self.read_stdout, daemon=True)
        self.reader.start()

    def read_stdout(self):
        for line in self.process.stdout:
            try:
                message = json.loads(line)
                if (
                    not isinstance(message, dict)
                    or message.get("jsonrpc") != "2.0"
                ):
                    raise ValueError("stdout contains a non-JSON-RPC message")
                self.messages.put(message)
            except ValueError as error:
                self.stdout_errors.append(str(error))
                self.messages.put(error)
        self.messages.put(EOFError("server closed stdout"))

    def send(self, method, params=None, request_id=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if request_id is not None:
            message["id"] = request_id
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.next_id += 1
        self.send(method, params, self.next_id)
        deadline = time.monotonic() + 10
        while True:
            try:
                message = self.messages.get(
                    timeout=max(0, deadline - time.monotonic())
                )
            except queue.Empty:
                raise AssertionError(f"Timed out awaiting {method}") from None
            if isinstance(message, Exception):
                raise AssertionError(f"Invalid response to {method}: {message}")
            if message.get("id") == self.next_id:
                if "error" in message:
                    raise AssertionError(f"{method} failed: {message['error']}")
                return message["result"]

    def initialize(self):
        result = self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {
                    "name": "offline-native-regression",
                    "version": "1",
                },
            },
        )
        self.send("notifications/initialized")
        return result

    def finish(self, terminate=False):
        if terminate:
            # Keep stdin open: a signal must also stop an idle MCP server.
            self.process.terminate()
        else:
            self.process.stdin.close()
        self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        if self.reader.is_alive():
            raise AssertionError("stdout reader did not finish after shutdown")
        if self.stdout_errors:
            raise AssertionError(self.stdout_errors)
        self.stderr.seek(0)
        return self.process.returncode, self.stderr.read()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.stderr.close()


class McpHandler(BaseHTTPRequestHandler):
    """Local Streamable HTTP fixture, including JSON and SSE responses."""

    def log_message(self, *args):
        pass

    def record(self, message=None):
        self.server.observed.append(
            {
                "method": self.command,
                "message": message,
                "authorization": self.headers.get("Authorization"),
                "session": self.headers.get("Mcp-Session-Id"),
            }
        )

    def reply(
        self, status, body=b"", content_type="application/json", session=False
    ):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if session:
            self.send_header("Mcp-Session-Id", "native-fixture-session")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.record()
        self.reply(405)

    def do_DELETE(self):
        self.record()
        self.reply(200)

    def do_POST(self):
        message = json.loads(
            self.rfile.read(int(self.headers["Content-Length"]))
        )
        self.record(message)
        method = message["method"]
        if "id" not in message:
            self.reply(202)
            return
        if method == "initialize":
            result = {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "local-fixture", "version": "1"},
            }
        elif method == "tools/list":
            result = {
                "tools": [
                    {
                        "name": "echo",
                        "description": "Return the fixture argument",
                        "inputSchema": {
                            "type": "object",
                            "properties": {"text": {"type": "string"}},
                        },
                    }
                ]
            }
        elif method == "tools/call" and message["params"]["name"] == "echo":
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": message["params"]["arguments"]["text"],
                    }
                ]
            }
        else:
            self.reply(400)
            return
        body = json.dumps(
            {"jsonrpc": "2.0", "id": message["id"], "result": result}
        ).encode()
        if method == "tools/list":
            self.reply(
                200,
                b"event: message\ndata: " + body + b"\n\n",
                "text/event-stream",
            )
        else:
            self.reply(200, body, session=method == "initialize")


class NativeMcpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get("MCP_TEST_HOME"):
            raise RuntimeError(
                "Set MCP_TEST_HOME to a built Home Manager activation package"
            )
        cls.home = Path(os.environ["MCP_TEST_HOME"]).resolve()
        if cls.home.parent != Path("/nix/store"):
            raise RuntimeError(
                "MCP_TEST_HOME must resolve to an immutable /nix/store output"
            )

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-mcp-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def slack_configuration(self, refresh=False):
        source = (self.home / "home-files/.local/bin/mcp-slack").read_text()
        stub = self.directory / "record-slack-config"
        stub.write_text(
            f"#!{sys.executable}\nimport json, os, sys\n"
            f"print(json.dumps({{'env': {{key: os.environ.get(key) "
            f"for key in {SLACK_ENV_KEYS!r}}}, "
            "'args': sys.argv[1:]}))\n"
        )
        stub.chmod(0o700)
        source, replacements = re.subn(
            r"(?m)^(\s*exec )/nix/store/[^\s]+/bin/slack-mcp-server(?=\s|$)",
            lambda match: match[1] + shlex.quote(str(stub)),
            source,
        )
        self.assertEqual(
            replacements,
            1,
            "Wrapper must execute exactly one pinned native Slack server",
        )
        wrapper = self.directory / "mcp-slack"
        wrapper.write_text(source)
        wrapper.chmod(0o700)
        env = isolated_env(
            self.directory,
            SLACK_XOXC="xoxc-synthetic-env",
            SLACK_COOKIE_D="xoxd-synthetic-env",
            SLACK_TEAM_ID="T_SYNTHETIC",
            SLACK_MCP_XOXP_TOKEN="xoxp-synthetic-conflict",
            SLACK_MCP_XOXB_TOKEN="xoxb-synthetic-conflict",
        )
        if refresh:
            session_file = (
                Path(env["XDG_CONFIG_HOME"]) / "slack-session/tokens.env"
            )
            session_file.parent.mkdir(parents=True)
            session_file.write_text(
                "SLACK_XOXC=xoxc-synthetic-refresh\n"
                "SLACK_COOKIE_D=xoxd-synthetic-refresh\n"
                f"SLACK_XOXC=$(touch {self.directory / 'executed'})\n"
                f"touch {self.directory / 'executed'}\n"
                "PATH=/invalid\n"
            )
        result = subprocess.run(
            [str(wrapper)],
            env=env,
            cwd=self.directory,
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        self.assertEqual(result.stderr, "")
        configuration = json.loads(result.stdout)
        self.assertFalse((self.directory / "executed").exists())
        expected_suffix = "refresh" if refresh else "env"
        self.assertEqual(
            configuration["env"]["SLACK_MCP_XOXC_TOKEN"],
            f"xoxc-synthetic-{expected_suffix}",
        )
        self.assertEqual(
            configuration["env"]["SLACK_MCP_XOXD_TOKEN"],
            f"xoxd-synthetic-{expected_suffix}",
        )
        self.assertEqual(
            configuration["env"]["SLACK_MCP_ADD_MESSAGE_TOOL"], "true"
        )
        self.assertEqual(
            configuration["env"]["SLACK_MCP_REACTION_TOOL"], "true"
        )
        self.assertIsNone(configuration["env"]["SLACK_MCP_XOXP_TOKEN"])
        self.assertIsNone(configuration["env"]["SLACK_MCP_XOXB_TOKEN"])
        args = configuration["args"]
        self.assertEqual(args.count("--enabled-tools"), 1)
        self.assertEqual(
            set(args[args.index("--enabled-tools") + 1].split(",")), SLACK_TOOLS
        )
        return configuration

    def test_slack_wrapper_maps_existing_session_credentials(self):
        self.slack_configuration()

    def test_slack_refresh_file_overrides_existing_environment(self):
        self.slack_configuration(refresh=True)

    def test_native_slack_advertises_configured_tools(self):
        configuration = self.slack_configuration()
        env = isolated_env(
            self.directory,
            **{
                key: value
                for key, value in configuration["env"].items()
                if value is not None
            },
        )
        env.update(SLACK_MCP_XOXC_TOKEN="demo", SLACK_MCP_XOXD_TOKEN="demo")
        # Demo mode avoids auth calls; --no-cache avoids startup API reads.
        client = StdioClient(
            [
                str(self.home / "home-path/bin/slack-mcp-server"),
                "--no-cache",
                *configuration["args"],
            ],
            env,
            self.directory,
        )
        self.addCleanup(client.close)
        initialized = client.initialize()
        self.assertIn("tools", initialized["capabilities"])
        tools = client.request("tools/list")["tools"]
        self.assertEqual({tool["name"] for tool in tools}, SLACK_TOOLS)
        self.assertEqual(client.finish()[0], 0)

    def test_native_proxy_auth_streamable_http_and_idle_shutdown(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), McpHandler)
        server.observed = []
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(server.server_close)
        self.addCleanup(worker.join, 5)
        self.addCleanup(server.shutdown)
        bearer = "Bearer synthetic-native-proxy-token"
        command = [
            str(self.home / "home-path/bin/mcp-remote-go"),
            "--server",
            f"http://127.0.0.1:{server.server_port}/mcp",
            "--transport",
            "streamable-http",
            "--allow-http",
        ]
        client = StdioClient(
            command,
            isolated_env(self.directory, MCP_AUTH_HEADER=bearer),
            self.directory,
        )
        self.addCleanup(client.close)
        self.assertEqual(
            client.initialize()["serverInfo"]["name"], "local-fixture"
        )
        self.assertEqual(
            client.request("tools/list")["tools"][0]["name"], "echo"
        )
        response = client.request(
            "tools/call",
            {"name": "echo", "arguments": {"text": "offline round trip"}},
        )
        self.assertEqual(
            response["content"],
            [{"type": "text", "text": "offline round trip"}],
        )
        exit_code, stderr = client.finish(terminate=True)
        self.assertEqual(exit_code, 0)
        self.assertNotIn(bearer, stderr)
        self.assertNotIn(bearer, " ".join(command))
        posts = [
            entry for entry in server.observed if entry["method"] == "POST"
        ]
        self.assertEqual(
            [entry["message"]["method"] for entry in posts],
            [
                "initialize",
                "notifications/initialized",
                "tools/list",
                "tools/call",
            ],
        )
        for entry in server.observed:
            self.assertEqual(entry["authorization"], bearer)
        for entry in posts[1:]:
            self.assertEqual(entry["session"], "native-fixture-session")
        self.assertIn("DELETE", [entry["method"] for entry in server.observed])


if __name__ == "__main__":
    unittest.main(verbosity=2)
