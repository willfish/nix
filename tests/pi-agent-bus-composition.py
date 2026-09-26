#!/usr/bin/env python3
"""Explicit candidate-only Bun/capture composition, \
with no builds or credentials.

Run the upstream full pi-runtime.test.mjs separately with these same selected
Pi/client/hub artifacts. This probe is the dotfiles-specific capture boundary,
not a replacement for its TUI lifecycle and exclusion suite.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import fcntl
import json
import os
import pty
import select
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import termios
import time

from test_pi_agent_bus_wiring import ROOT, wrapper_text


def store_path(value):
    path = Path(value).resolve(strict=True)
    if not str(path).startswith("/nix/store/"):
        raise argparse.ArgumentTypeError("select an immutable Nix artifact")
    return path


def exclusion_composition(
    root, raw, env, extension, history, received, wrapper, capture_dir
):
    ("Pinned Bun exclusion CLI, synthetic composition, "
     "with real history/client.")
    fixture = ROOT / "tests/fixtures/pi-agent-bus-composition"
    cases = [
        ("ordinary-qwen-model", [], {}, True, True),
        ("qwen-style", ["--offline", "--no-extensions"], {}, False, False),
        ("discovery-suppressed", ["--no-extensions"], {}, False, False),
        (
            "explicit-offline",
            ["--offline", "--no-extensions", "-e", str(extension / "index.ts")],
            {},
            False,
            False,
        ),
        ("disabled", [], {"PI_AGENT_BUS_ENABLED": "0"}, False, True),
        ("offline-env", [], {"PI_OFFLINE": "1"}, False, True),
        ("capture-canonical", [], {"CAPTURE_PROMPTS": "1"}, True, True),
        (
            "capture-abbreviated-ip",
            [],
            {
                "CAPTURE_PROMPTS": "1",
                "PI_AGENT_BUS_URL": (
                    env["PI_AGENT_BUS_URL"].replace("127.0.0.2", "127.2")
                ),
            },
            False,
            True,
        ),
        (
            "capture-integer-ip",
            [],
            {
                "CAPTURE_PROMPTS": "1",
                "PI_AGENT_BUS_URL": (
                    env["PI_AGENT_BUS_URL"].replace("127.0.0.2", "2130706434")
                ),
            },
            False,
            True,
        ),
    ]
    for name, flags, overrides, participates, discovered in cases:
        home = root / name
        agent = home / "agent"
        extensions = agent / "extensions"
        extensions.mkdir(parents=True)
        (extensions / "agent-bus").symlink_to(
            extension, target_is_directory=True
        )
        (extensions / "prompt-history").symlink_to(
            history, target_is_directory=True
        )
        (extensions / "sentinel.ts").write_text(
            'import {appendFileSync} from "node:fs"; export default'
            ' function(pi) {pi.on("session_start", () =>'
            ' appendFileSync(process.env.COMPOSITION_EVENTS,'
            'JSON.stringify({type:"discovered"})'
            ' + "\\n")); }'
        )
        (agent / "settings.json").write_text(
            json.dumps(
                {
                    "defaultProvider": "fixture",
                    "defaultModel": "qwen-fixture",
                    "defaultThinkingLevel": "off",
                    "enableInstallTelemetry": False,
                    "lastChangelogVersion": "0.85.1",
                    "packages": [],
                }
            )
        )
        (agent / "models.json").write_text(
            json.dumps(
                {
                    "providers": {
                        "fixture": {
                            "baseUrl": env["COMPOSITION_PROVIDER_URL"],
                            "api": "openai-completions",
                            "apiKey": "synthetic-provider-token",
                            "models": [
                                {
                                    "id": "qwen-fixture",
                                    "contextWindow": 128000,
                                    "maxTokens": 1024,
                                }
                            ],
                        }
                    }
                }
            )
        )
        events = home / "events"
        events.touch()
        master, slave = pty.openpty()
        fcntl.ioctl(
            slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 120, 0, 0)
        )
        baseline = len(received["bus"])
        child = subprocess.Popen(
            [
                str(wrapper if name.startswith("capture-") else raw),
                "--no-context-files",
                "--no-skills",
                "--no-prompt-templates",
                "--no-themes",
                "--no-builtin-tools",
                "--no-session",
                "--no-approve",
                "-e",
                str(fixture / "observer.ts"),
                *flags,
            ],
            cwd=home,
            env={
                **env,
                "HOME": str(home),
                "PI_CODING_AGENT_DIR": str(agent),
                "TERM": "xterm-256color",
                "COMPOSITION_EVENTS": str(events),
                "CAPTURE_PROMPTS": "0",
                **overrides,
            },
            stdin=slave,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
        )
        os.close(slave)
        output = b""

        def drain(seconds):
            nonlocal output
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                assert (
                    child.poll() is None
                ), f"{name}: early exit: {output.decode(errors='replace')}"
                if select.select([master], [], [], 0.05)[0]:
                    chunk = os.read(master, 65536)
                    output = (output + chunk)[-131072:]
                    if b"\x1b]11;?" in chunk:
                        os.write(master, b"\x1b]11;rgb:1a1a/1b1b/2626\x1b\\")

        try:
            deadline = time.monotonic() + 10
            while (
                '"started"' not in events.read_text()
                or b"qwen-fixture" not in output
            ):
                assert (
                    time.monotonic() < deadline
                ), f"{name}: startup deadline: {output!r}"
                drain(0.1)
            os.write(master, b"/composition-ping")
            drain(0.2)
            os.write(master, b"\r")
            deadline = time.monotonic() + 5
            while '"pong"' not in events.read_text():
                assert (
                    time.monotonic() < deadline
                ), f"{name}: unresponsive TUI: {output!r}"
                drain(0.1)
            drain(
                5.5
            )  # Observe beyond a heartbeat, not merely a final empty list.
            records = [
                json.loads(line) for line in events.read_text().splitlines()
            ]
            assert all(r.get("mode", "tui") == "tui" for r in records)
            assert any(r["type"] == "discovered" for r in records) == discovered
            attempts = received["bus"][baseline:]
            if name.startswith("capture-"):
                logs = (capture_dir / "pi.jsonl").read_text() + (
                    capture_dir / "pi-server.log"
                ).read_text()
                assert (
                    "/v1/agents/" not in logs and "/v1/events?" not in logs
                ), "actual Switchboard traffic entered capture"
            if participates:
                puts = [
                    json.loads(body)
                    for method, _, body, _ in attempts
                    if method == "PUT"
                ]
                assert puts and puts[0]["model"] == {
                    "provider": "fixture",
                    "id": "qwen-fixture",
                }
                assert any(
                    method == "GET" and path.startswith("/v1/events?")
                    for method, path, _, _ in attempts
                ), "actual Switchboard SSE not opened"
            else:
                assert not attempts, f"{name}: unexpected bus request attempts"
            assert not any(
                marker in output
                for marker in (
                    b"Failed to load extension",
                    b"Extension error",
                    b"not found. Downloading",
                )
            ), output
            print(
                f"PASS {name}: responsive TUI, discovery={discovered},"
                f" participation-attempts={participates}"
            )
        finally:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=7)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=3)
                raise AssertionError(f"{name}: unbounded Pi cleanup")
            finally:
                os.close(master)

    # Unlike TUI exclusions, print/JSON complete normally after synthetic
    # handled input. RPC must stay responsive until explicitly terminated.
    for mode, flags in (
        ("print", ["-p", "composition-headless"]),
        ("json", ["--mode", "json", "composition-headless"]),
        ("rpc", ["--mode", "rpc"]),
    ):
        events.write_text("")
        baseline = len(received["bus"])
        child = subprocess.Popen(
            [
                str(raw),
                "--no-context-files",
                "--no-skills",
                "--no-prompt-templates",
                "--no-themes",
                "--no-builtin-tools",
                "--no-session",
                "--no-approve",
                "-e",
                str(fixture / "observer.ts"),
                *flags,
            ],
            cwd=home,
            env={
                **env,
                "HOME": str(home),
                "PI_CODING_AGENT_DIR": str(agent),
                "COMPOSITION_EVENTS": str(events),
                "CAPTURE_PROMPTS": "0",
            },
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            if mode == "rpc":
                child.stdin.write(
                    json.dumps(
                        {"type": "prompt", "message": "composition-headless"}
                    )
                    + "\n"
                )
                child.stdin.flush()
                deadline = time.monotonic() + 10
                while '"handled"' not in events.read_text():
                    assert (
                        child.poll() is None
                    ), "RPC exited before handling fixture input"
                    assert (
                        time.monotonic() < deadline
                    ), "RPC startup/input deadline"
                    time.sleep(0.05)
                time.sleep(5.5)
                assert child.poll() is None, "RPC exited during observation"
                child.terminate()
                output = child.communicate(timeout=5)[0]
            else:
                output = child.communicate(timeout=15)[0]
                assert child.returncode == 0, output
                time.sleep(5.5)
            records = [
                json.loads(line) for line in events.read_text().splitlines()
            ]
            assert {r["type"] for r in records} >= {
                "started",
                "handled",
                "discovered",
            }, records
            assert all(r.get("mode", mode) == mode for r in records), records
            assert not received["bus"][
                baseline:
            ], f"{mode}: unexpected bus attempts"
            assert not any(
                marker in output
                for marker in (
                    "Failed to load extension",
                    "Extension error",
                    "not found. Downloading",
                )
            ), output
            print(
                f"PASS {mode}: synthetic input handled, discovery=True,"
                " participation-attempts=False"
            )
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate(timeout=3)
                    raise AssertionError(f"{mode}: unbounded Pi cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-package", required=True, type=store_path)
    parser.add_argument("--pi-version", required=True)
    parser.add_argument("--extension-package", required=True, type=store_path)
    parser.add_argument("--prompt-history", required=True, type=store_path)
    parser.add_argument("--mitmdump", required=True, type=store_path)
    parser.add_argument("--ca-bundle", required=True, type=store_path)
    parser.add_argument(
        "--tool-path",
        required=True,
        help="Nix bin directories supplying fd/ripgrep and shell helpers",
    )
    args = parser.parse_args()
    if not all(
        p.startswith("/nix/store/") and p.endswith("/bin")
        for p in args.tool_path.split(":")
    ):
        parser.error("tool-path must contain only explicit Nix bin directories")

    def tool(name):
        value = shutil.which(name, path=args.tool_path)
        if not value:
            parser.error(f"tool-path must supply {name}")
        return str(Path(value).resolve(strict=True))

    bash = tool("bash")
    flock = tool("flock")
    pi_dir = args.pi_package / "libexec/pi"
    raw = pi_dir / "pi"
    with raw.open("rb") as handle:
        assert handle.read(4).hex() in (
            "7f454c46",
            "cffaedfe",
            "feedfacf",
            "cafebabe",
        ), "reject credential/script wrappers"
    assert (
        json.loads((pi_dir / "package.json").read_text())["version"]
        == args.pi_version
    )
    manifest = json.loads((args.extension_package / "package.json").read_text())
    assert manifest["name"] == "pi-switchboard"
    assert manifest["pi"]["extensions"] == ["./index.ts"]
    assert (args.prompt_history / "index.ts").is_file()
    with tempfile.TemporaryDirectory(prefix="bus-composition-") as directory:
        root = Path(directory)
        agent = root / "agent"
        agent.mkdir()
        (agent / "settings.json").write_text(
            json.dumps(
                {
                    "enableInstallTelemetry": False,
                    "packages": [],
                }
            )
        )
        received = {"bus": [], "provider": []}
        servers = []
        threads = []

        def server(kind, address):
            class Handler(BaseHTTPRequestHandler):
                def log_message(self, *_args):
                    pass

                def handle_request(self):
                    body = self.rfile.read(
                        int(self.headers.get("Content-Length", 0))
                    )
                    received[kind].append(
                        (
                            self.command,
                            self.path,
                            body,
                            self.headers.get("Authorization"),
                        )
                    )
                    if kind == "bus" and self.path.startswith("/v1/events?"):
                        self.send_response(200)
                        self.send_header("Content-Type", "text/event-stream")
                        self.end_headers()
                        try:
                            while True:
                                self.wfile.write(b": synthetic keepalive\n\n")
                                self.wfile.flush()
                                time.sleep(0.2)
                        except (BrokenPipeError, ConnectionResetError):
                            return
                    if (
                        kind == "bus"
                        and self.path.startswith("/v1/agents/")
                        and not self.path.endswith("/synthetic")
                    ):
                        self.send_response(204)
                        self.end_headers()
                        return
                    data = (
                        "synthetic-direct-bus"
                        if kind == "bus"
                        else "synthetic-provider-response"
                    ).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)

                do_GET = do_PUT = do_POST = do_DELETE = handle_request

            http = ThreadingHTTPServer((address, 0), Handler)
            worker = threading.Thread(target=http.serve_forever, daemon=True)
            worker.start()
            servers.append(http)
            threads.append(worker)
            return f"http://{address}:{http.server_port}"

        try:
            # Not localhost/127.0.0.1: those are already bypassed by capture.sh.
            bus_url = server("bus", "127.0.0.2")
            provider_url = server("provider", "127.0.0.3")
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            capture = root / "capture"
            capture.write_text(
                "#!"
                + bash
                + "\n"
                + (ROOT / "home/user/prompt-capture.sh")
                .read_text()
                .replace("pi) port=8302", f"pi) port={port}")
                .replace("/etc/ssl/certs/ca-bundle.crt", str(args.ca_bundle))
            )
            capture.chmod(0o700)
            secret = root / "secret"
            secret.write_text(
                "#!" + bash + "\nprintf 'synthetic-composition-token'\n"
            )
            secret.chmod(0o700)
            wrapper = root / "wrapper"
            # Execute only the source shell rendered with synthetic helpers.
            # Never source/execute home-files/.local/bin/pi or credential
            # wrappers.
            shell = wrapper_text(
                secret_helper=secret, pi=raw, capture=capture, bash=bash
            )
            shell = shell.replace(
                "--theme fixture-light --theme fixture-dark --use-theme"
                " fixture-light/fixture-dark",
                "",
            )
            wrapper.write_text(shell)
            wrapper.chmod(0o700)
            env = {
                "HOME": directory,
                "TMPDIR": directory,
                "PATH": args.tool_path,
                "XDG_CONFIG_HOME": str(root / "config"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_DATA_HOME": str(root / "data"),
                "PI_CODING_AGENT_DIR": str(agent),
                "PI_PACKAGE_DIR": str(pi_dir),
                "PI_SKIP_VERSION_CHECK": "1",
                "PI_TELEMETRY": "0",
                "CAPTURE_PROMPTS": "1",
                "MITMDUMP": str(args.mitmdump),
                "FLOCK": flock,
                "PI_AGENT_BUS_URL": bus_url,
                "PI_AGENT_BUS_TOKEN": "synthetic-composition-token",
                "COMPOSITION_PROVIDER_URL": (
                    provider_url + "/synthetic-provider-capture"
                ),
                "NO_PROXY": "upper.invalid",
                "no_proxy": "lower.invalid",
            }
            # Exercise the actual client's URL parser under Bun, but never its
            # runtime for default/external names. No network-capable entry
            # loads.
            policy_capture = root / "policy-capture"
            policy_capture.write_text("#!" + bash + '\nshift 2\nexec "$@"\n')
            policy_capture.chmod(0o700)
            policy_wrapper = root / "policy-wrapper"
            policy_wrapper.write_text(
                wrapper_text(
                    secret_helper=secret,
                    pi=raw,
                    capture=policy_capture,
                    bash=bash,
                ).replace(
                    "--theme fixture-light --theme fixture-dark --use-theme"
                    " fixture-light/fixture-dark",
                    "",
                )
            )
            policy_wrapper.chmod(0o700)
            policy_cases = [
                ("", "terminus", False),
                ("http://bücher.example:7420", "xn--bcher-kva.example", True),
                ("http://127.2:7420", "127.0.0.2", True),
                ("http://2130706434:7420", "127.0.0.2", True),
                ("http://0177.0.0.2:7420", "127.0.0.2", True),
                ("http://0x7f000002:7420", "127.0.0.2", True),
                ("http://[::1]:7420", "[::1]", True),
                ("http://BUS.EXAMPLE:7420", "bus.example", False),
            ]
            for url, host, disabled in policy_cases:
                result = subprocess.run(
                    [
                        str(policy_wrapper),
                        "--offline",
                        "--no-extensions",
                        "--no-skills",
                        "--no-context-files",
                        "--no-prompt-templates",
                        "--no-themes",
                        "-e",
                        str(
                            ROOT
                            / ("tests/fixtures/pi-agent-bus-composition/"
                               "url-policy.ts")
                        ),
                        "--list-models",
                        "__url_policy_fixture__",
                    ],
                    cwd=root,
                    env={
                        **env,
                        "PI_AGENT_BUS_URL": url,
                        "COMPOSITION_EXTENSION_PROTOCOL": str(
                            args.extension_package / "extension/protocol.ts"
                        ),
                        "COMPOSITION_EXPECT_HOST": host,
                        "COMPOSITION_EXPECT_DISABLED": "1" if disabled else "0",
                    },
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                assert result.returncode == 0, result.stdout + result.stderr
                assert "COMPOSITION_URL_POLICY_PASSED" in result.stdout, (
                    result.stdout + result.stderr
                )
            assert received == {
                "bus": [],
                "provider": [],
            }, "URL policy checks must not send requests"
            print(
                "PASS Bun/client URL parsing: default, Unicode, IPv4 spellings"
                " and IPv6; no client runtime or network"
            )

            child = subprocess.Popen(
                [
                    str(wrapper),
                    "--offline",
                    "--no-extensions",
                    "--no-skills",
                    "--no-context-files",
                    "--no-prompt-templates",
                    "--no-themes",
                    "--no-session",
                    "-e",
                    str(args.extension_package / "index.ts"),
                    "-e",
                    str(args.prompt_history / "index.ts"),
                    "-e",
                    str(
                        ROOT
                        / "tests/fixtures/pi-agent-bus-composition/probe.ts"
                    ),
                    "--list-models",
                    "__composition_fixture__",
                ],
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
            try:
                output = child.communicate(timeout=40)[0]
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.communicate(timeout=7)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate(timeout=3)
                raise AssertionError(
                    "capture fixture deadline; terminated process group"
                )
            assert child.returncode == 0, output
            assert "COMPOSITION_BUN_CAPTURE_PASSED" in output, output
            assert not any(
                marker in output
                for marker in (
                    "Failed to load extension",
                    "Extension error",
                    "not found. Downloading",
                )
            ), output
            assert len(received["bus"]) == 2, received["bus"]
            assert len(received["provider"]) == 1, received["provider"]
            for _, _, body, authorization in received["bus"]:
                assert b"synthetic-bus-private-body" in body
                assert authorization == "Bearer synthetic-composition-token"
            capture_dir = root / "state/prompt-capture"
            log = (capture_dir / "pi.jsonl").read_text()
            assert (
                "synthetic-provider-capture" in log
            ), "provider must traverse capture"
            all_logs = (
                log + output + (capture_dir / "pi-server.log").read_text()
            )
            for marker in (
                "synthetic-bus-private-body",
                "synthetic-composition-token",
                "synthetic-direct-bus",
                "/v1/agents/synthetic",
                "/v1/messages",
            ):
                assert (
                    marker not in all_logs
                ), "bus content entered capture/output"
            assert not (
                capture_dir / "pi.pid"
            ).exists(), "capture child not cleaned up"
            with socket.socket() as sock:
                assert (
                    sock.connect_ex(("127.0.0.1", port)) != 0
                ), "proxy listener leaked"
            print(
                "PASS selected raw Bun + external client + prompt-history load;"
                " synthetic bus bypass and provider capture; cleanup"
            )
            exclusion_composition(
                root,
                raw,
                env,
                args.extension_package,
                args.prompt_history,
                received,
                wrapper,
                capture_dir,
            )
        finally:
            for http in servers:
                http.shutdown()
                http.server_close()
            for worker in threads:
                worker.join(timeout=3)


if __name__ == "__main__":
    main()
