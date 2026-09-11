"""Offline MCP integration through the actual Pi runtime and local fixtures.

PI_MCP_TEST_EXTENSION must name the packaged adapter's index.ts.
PI_MCP_TEST_BIN optionally selects the packaged Pi executable (default: pi).
Run with Python's standard library: python3 tests/pi-mcp-runtime.py -v
The adapter must include settings.namespaceTools support for the gateway-only
regression. Temporary profiles and whitelisted child environments avoid loading
live authentication or configuration. Only a loopback model fixture and a
local Python MCP process are used, with synthetic credentials and no hosted
inference. For OS-enforced isolation, run the whole suite inside a network
namespace with loopback enabled; both fixtures must share Pi's namespace.
"""

import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ACTIONS = {
    "status": {},
    "connect": {"connect": "fixture"},
    "describe": {"describe": "fixture_echo"},
    "call": {"tool": "fixture_echo", "args": {"text": "offline success"}},
    "error": {"tool": "fixture_fail", "args": {}},
    "recover": {"tool": "fixture_echo", "args": {"text": "recovered"}},
    "refresh": {"refresh": True},
}


def fixture_server(event_path):
    """Small legacy MCP server with observable startup and shutdown."""

    def record(event):
        with open(event_path, "a", encoding="utf-8") as stream:
            stream.write(
                json.dumps({"event": event, "pid": os.getpid()}) + "\n"
            )

    def stop(*_):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    record("started")
    try:
        for line in sys.stdin:
            message = json.loads(line)
            if "id" not in message:
                continue
            method = message["method"]
            response = {"jsonrpc": "2.0", "id": message["id"]}
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "offline-fixture", "version": "1"},
                }
            elif method == "tools/list":
                result = {
                    "tools": [
                        {
                            "name": "echo",
                            "description": "Echo supplied text.",
                            "inputSchema": {
                                "type": "object",
                                "properties": {"text": {"type": "string"}},
                                "required": ["text"],
                            },
                        },
                        {
                            "name": "fail",
                            "description": "Return a fixture error.",
                            "inputSchema": {"type": "object", "properties": {}},
                        },
                    ]
                }
            elif method == "tools/call":
                params = message["params"]
                failed = params["name"] == "fail"
                result = {
                    "content": [
                        {
                            "type": "text",
                            "text": (
                                "expected fixture failure"
                                if failed
                                else params["arguments"]["text"]
                            ),
                        }
                    ],
                    "isError": failed,
                }
                record("error" if failed else "called")
            elif method == "ping":
                result = {}
            else:
                response["error"] = {
                    "code": -32601,
                    "message": "Method not found",
                }
                result = None
            if result is not None:
                response["result"] = result
            print(json.dumps(response), flush=True)
    finally:
        record("stopped")


class ModelHandler(BaseHTTPRequestHandler):
    """Streaming model requests one deterministic MCP action per turn."""

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(body)
        messages = body["messages"]
        if messages[-1]["role"] == "tool" or getattr(
            self.server, "final_only", False):
            delta = {"role": "assistant", "content": "Fixture turn complete."}
            finish = "stop"
        else:
            action = next(
                m["content"] for m in reversed(messages) if m["role"] == "user"
            )
            if isinstance(action, list):
                action = "".join(x.get("text", "") for x in action)
            tool_name, arguments = (
                ("mcp", ACTIONS[action]) if action in ACTIONS
                else (json.loads(action)["tool"], json.loads(action)["args"])
            )
            delta = {
                "role": "assistant",
                "tool_calls": [
                    {
                        "index": 0,
                        "id": f"call_{len(self.server.requests)}",
                        "type": "function",
                        "function": {
                            "name": tool_name,
                            "arguments": json.dumps(arguments),
                        },
                    }
                ],
            }
            finish = "tool_calls"
        chunks = [
            {
                "id": "offline",
                "object": "chat.completion.chunk",
                "created": 1,
                "model": "fixture",
                "choices": [
                    {"index": 0, "delta": delta, "finish_reason": None}
                ],
            },
            {
                "id": "offline",
                "object": "chat.completion.chunk",
                "created": 1,
                "model": "fixture",
                "choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
            },
        ]
        data = "".join(f"data: {json.dumps(chunk)}\n\n" for chunk in chunks)
        data += "data: [DONE]\n\n"
        encoded = data.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


class PiClient:
    def __init__(self, command, env, directory):
        self.events = queue.Queue()
        self.observed = []
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
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            try:
                self.events.put(json.loads(line))
            except ValueError:
                self.events.put(
                    AssertionError(f"Non-JSON RPC output: {line[:200]}")
                )
        self.events.put(EOFError("Pi closed stdout"))

    def diagnostics(self):
        self.stderr.seek(0)
        return self.stderr.read() + json.dumps(self.observed[:6])

    def send(self, **message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def until(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        observed = []
        while True:
            try:
                event = self.events.get(
                    timeout=max(0, deadline - time.monotonic())
                )
            except queue.Empty:
                raise AssertionError(
                    f"Timed out waiting for Pi: {self.diagnostics()}"
                ) from None
            if isinstance(event, Exception):
                raise AssertionError(f"{event}: {self.diagnostics()}")
            observed.append(event)
            self.observed.append(event)
            if predicate(event):
                return observed

    def prompt(self, action, tool_name="mcp"):
        self.send(type="prompt", id=action, message=action)
        events = self.until(lambda event: event.get("type") == "agent_end")
        failures = [
            event
            for event in events
            if event.get("type") == "response" and event.get("success") is False
        ]
        if failures:
            raise AssertionError(failures)
        results = [
            event
            for event in events
            if event.get("type") == "tool_execution_end"
        ]
        if len(results) != 1 or results[0].get("toolName") != tool_name:
            raise AssertionError(
                f"Expected one {tool_name} execution, got {results}: "
                f"{self.diagnostics()}"
            )
        return results[0]

    def finish(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)
        self.reader.join(timeout=5)
        if self.process.returncode != 0:
            raise AssertionError(self.diagnostics())

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.stderr.close()


class PiMcpRuntimeTest(unittest.TestCase):
    namespace_tools = True
    default_selection = False

    def test_actual_pi_discovers_calls_and_shuts_down_mcp(self):
        extension = Path(os.environ.get("PI_MCP_TEST_EXTENSION", ""))
        self.assertTrue(
            extension.is_file(),
            "Set PI_MCP_TEST_EXTENSION to packaged index.ts",
        )
        binary = os.environ.get("PI_MCP_TEST_BIN", "pi")
        for isolated in (False, True):
            with (
                self.subTest(isolated_profile=isolated),
                tempfile.TemporaryDirectory() as temp,
            ):
                directory = Path(temp)
                profile = directory / (
                    "isolated-agent" if isolated else ".pi/agent"
                )
                profile.mkdir(parents=True)
                config_dir = directory / ".config/mcp"
                config_dir.mkdir(parents=True)
                event_path = directory / "fixture-events.jsonl"
                config = {
                    "mcpServers": {
                        "fixture": {
                            "command": sys.executable,
                            "args": [
                                str(Path(__file__).resolve()),
                                "--fixture",
                                str(event_path),
                            ],
                            "lifecycle": "lazy",
                            "requestTimeoutMs": 5000,
                            "protocolVersion": "legacy",
                            "directTools": False,
                        }
                    },
                    "settings": {
                        "directTools": False,
                        "namespaceTools": self.namespace_tools,
                        "scriptMode": False,
                        "hostConfigDiscovery": "off",
                    },
                }
                (config_dir / "mcp.json").write_text(json.dumps(config))
                server = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
                server.requests = []
                thread = threading.Thread(
                    target=server.serve_forever, daemon=True
                )
                thread.start()
                self.addCleanup(server.server_close)
                self.addCleanup(server.shutdown)
                model = {
                    "providers": {
                        "fixture": {
                            "baseUrl": (
                                f"http://127.0.0.1:{server.server_port}/v1"
                            ),
                            "api": "openai-completions",
                            "apiKey": "synthetic-test-key",
                            "models": [
                                {
                                    "id": "fixture",
                                    "name": "Fixture",
                                    "reasoning": False,
                                    "input": ["text"],
                                    "contextWindow": 32768,
                                    "maxTokens": 1024,
                                    "cost": {
                                        "input": 0,
                                        "output": 0,
                                        "cacheRead": 0,
                                        "cacheWrite": 0,
                                    },
                                }
                            ],
                        }
                    }
                }
                (profile / "models.json").write_text(json.dumps(model))
                command = [
                    binary,
                    "--mode",
                    "rpc",
                    "--offline",
                    "--no-session",
                    "--provider",
                    "fixture",
                    "--model",
                    "fixture",
                    "--thinking",
                    "off",
                    "--no-context-files",
                    "--no-skills",
                    "--no-prompt-templates",
                    "--no-themes",
                    "--system-prompt",
                    "Run the requested fixture action.",
                    "--tools",
                    "read,bash,edit,write,mcp",
                ]
                if self.default_selection:
                    command = command[:-2]
                    reload_extension = directory / "reload.ts"
                    reload_extension.write_text(
                        'export default function(pi) { '
                        'pi.registerCommand("fixture-reload", '
                        '{handler: async (_args, ctx) => { '
                        'await ctx.reload(); }}); }'
                    )
                    command += ["--extension", str(reload_extension)]
                if isolated:
                    command += [
                        "--no-extensions",
                        "--extension",
                        str(extension.resolve()),
                    ]
                else:
                    extensions = profile / "extensions"
                    extensions.mkdir()
                    (extensions / "mcp").symlink_to(
                        extension.resolve().parent, target_is_directory=True
                    )
                env = {
                    "PATH": os.environ.get("PATH", os.defpath),
                    "HOME": temp,
                    "XDG_CONFIG_HOME": str(directory / ".config"),
                    "XDG_CACHE_HOME": str(directory / ".cache"),
                    "PI_TELEMETRY": "0",
                }
                if isolated:
                    env["PI_CODING_AGENT_DIR"] = str(profile)
                # The adapter discovers all configured servers once per profile.
                # Later sessions use cached metadata and defer process startup.
                cold = PiClient(command, env, directory)
                try:
                    self.assertIn(
                        "fixture",
                        json.dumps(cold.prompt("status")),
                        cold.diagnostics(),
                    )
                    self.assertTrue(
                        event_path.exists(),
                        "First session did not discover its server",
                    )
                    cold.finish()
                finally:
                    cold.close()
                first_events = [
                    json.loads(line)["event"]
                    for line in event_path.read_text().splitlines()
                ]
                self.assertEqual(first_events, ["started", "stopped"])
                self.assertTrue((profile / "mcp-cache.json").is_file())
                event_path.unlink()
                client = PiClient(command, env, directory)
                try:
                    status = client.prompt("status")
                    self.assertIn(
                        "fixture", json.dumps(status), client.diagnostics()
                    )
                    self.assertFalse(
                        event_path.exists(),
                        "Lazy server started before connect",
                    )
                    self.assertFalse(client.prompt("connect").get("isError"))
                    schema = json.dumps(client.prompt("describe"))
                    self.assertIn("text", schema)
                    self.assertIn("string", schema)
                    self.assertIn(
                        "offline success", json.dumps(client.prompt("call"))
                    )
                    error = client.prompt("error")
                    self.assertIn("expected fixture failure", json.dumps(error))
                    self.assertTrue(
                        error.get("isError")
                        or error.get("result", {}).get("isError")
                    )
                    self.assertIn(
                        "recovered", json.dumps(client.prompt("recover"))
                    )
                    if self.default_selection:
                        self.assertFalse(
    client.prompt("refresh").get("isError"))
                        client.send(
    type="prompt",
    id="reload",
     message="/fixture-reload")
                        client.until(
                            lambda event: event.get("type") == "response"
                            and event.get("id") == "reload")
                        self.assertIn(
    "recovered", json.dumps(
        client.prompt("recover")))
                    for request in server.requests:
                        names = {
                            tool["function"]["name"]
                            for tool in request["tools"]
                        }
                        self.assertEqual(
                            names, {"read", "bash", "edit", "write", "mcp"}
                        )
                    client.finish()
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        events = [
                            json.loads(line)
                            for line in event_path.read_text().splitlines()
                        ]
                        if any(event["event"] == "stopped" for event in events):
                            break
                        time.sleep(0.05)
                    self.assertEqual(
                        [e["event"] for e in events],
                        (["started", "called", "error", "called", "stopped",
                          "started", "called", "stopped"]
                         if self.default_selection
                         else ["started", "called", "error",
                               "called", "stopped"]),
                    )
                finally:
                    client.close()
                    server.shutdown()
                    server.server_close()
                    thread.join(timeout=5)


class PiMcpGatewayOnlyRuntimeTest(PiMcpRuntimeTest):
    """Disabling namespace registration must work without a CLI tool
    allowlist.
    """

    namespace_tools = False
    default_selection = True


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--fixture":
        fixture_server(sys.argv[2])
    else:
        unittest.main()
