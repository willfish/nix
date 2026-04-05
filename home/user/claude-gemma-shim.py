#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8090
DEFAULT_UPSTREAM_HOST = "127.0.0.1"
DEFAULT_UPSTREAM_PORT = 8080


def env_int(name: str, default: int) -> int:
    value = os.environ.get(name)
    if value is None:
        return default
    try:
        return int(value)
    except ValueError as exc:
        raise SystemExit(f"{name} must be an integer, got {value!r}") from exc


def build_upstream_base_url(host: str, port: int) -> str:
    return f"http://{host}:{port}"


def json_bytes(payload: dict) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")


class ShimRequestHandler(BaseHTTPRequestHandler):
    server_version = "claude-gemma-shim/0.1"
    protocol_version = "HTTP/1.1"

    def _write_json(self, status_code: int, payload: dict) -> None:
        body = json_bytes(payload)
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _health_payload(self) -> dict:
        config = self.server.shim_config
        return {
            "ok": True,
            "service": "claude-gemma-shim",
            "localhost_only": True,
            "bind_address": config["host"],
            "port": config["port"],
            "upstream": {
                "base_url": config["upstream_base_url"],
                "messages_url": f"{config['upstream_base_url']}/v1/chat/completions",
            },
        }

    def _not_found(self) -> None:
        self._write_json(
            404,
            {
                "ok": False,
                "error": "not_found",
                "path": urlparse(self.path).path,
            },
        )

    def _not_implemented(self) -> None:
        self._write_json(
            501,
            {
                "ok": False,
                "error": "not_implemented",
                "message": "Anthropic request translation is not implemented yet.",
            },
        )

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        path = urlparse(self.path).path
        if path == "/health":
            self._write_json(200, self._health_payload())
            return
        self._not_found()

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        content_length = int(self.headers.get("Content-Length", "0") or "0")
        if content_length:
            self.rfile.read(content_length)
        self._not_implemented()

    def log_message(self, format: str, *args) -> None:  # noqa: A003 - stdlib hook
        return


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Localhost-only Claude Code shim for Gemma.",
    )
    parser.add_argument("--host", default=os.environ.get("CLAUDE_GEMMA_SHIM_HOST", DEFAULT_HOST))
    parser.add_argument(
        "--port",
        type=int,
        default=env_int("CLAUDE_GEMMA_SHIM_PORT", DEFAULT_PORT),
    )
    parser.add_argument(
        "--upstream-host",
        default=os.environ.get("LLM_GEMMA_HOST", DEFAULT_UPSTREAM_HOST),
    )
    parser.add_argument(
        "--upstream-port",
        type=int,
        default=env_int("LLM_GEMMA_PORT", DEFAULT_UPSTREAM_PORT),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.host != "127.0.0.1":
        raise SystemExit("Refusing to bind outside 127.0.0.1; this shim is localhost-only.")

    upstream_base_url = build_upstream_base_url(args.upstream_host, args.upstream_port)
    server = ThreadingHTTPServer((args.host, args.port), ShimRequestHandler)
    server.shim_config = {
        "host": args.host,
        "port": args.port,
        "upstream_base_url": upstream_base_url,
    }

    print(
        f"claude-gemma-shim listening on http://{args.host}:{args.port} "
        f"with upstream {upstream_base_url}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
