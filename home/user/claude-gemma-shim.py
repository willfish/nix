#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request
from urllib.parse import urlparse


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8090
DEFAULT_UPSTREAM_HOST = "127.0.0.1"
DEFAULT_UPSTREAM_PORT = 8080
UPSTREAM_MESSAGES_PATH = "/v1/chat/completions"
ALLOWED_REQUEST_FIELDS = {"max_tokens", "messages", "model", "stream", "system"}
UNSUPPORTED_REQUEST_FIELDS = {
    "metadata",
    "stop_sequences",
    "temperature",
    "thinking",
    "tool_choice",
    "tools",
    "top_k",
    "top_p",
}
SUPPORTED_MESSAGE_ROLES = {"assistant", "user"}
SUPPORTED_CONTENT_BLOCK_TYPE = "text"
ANTHROPIC_API_VERSION = "2023-06-01"


class RequestValidationError(Exception):
    def __init__(self, status_code: int, error_type: str, message: str) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.error_type = error_type
        self.message = message

    def to_payload(self) -> dict:
        return {
            "type": "error",
            "error": {
                "type": self.error_type,
                "message": self.message,
            },
        }


class UpstreamRequestError(Exception):
    def __init__(self, message: str, *, upstream_status: int | None = None, upstream_body: str | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.upstream_status = upstream_status
        self.upstream_body = upstream_body

    def to_payload(self) -> dict:
        payload = {
            "type": "error",
            "error": {
                "type": "api_error",
                "message": self.message,
            },
        }
        if self.upstream_status is not None:
            payload["error"]["upstream_status"] = self.upstream_status
        if self.upstream_body:
            payload["error"]["upstream_body"] = self.upstream_body
        return payload


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


def require_object(value: object, *, context: str) -> dict:
    if not isinstance(value, dict):
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be an object")
    return value


def require_string(value: object, *, context: str) -> str:
    if not isinstance(value, str):
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be a string")
    return value


def require_bool(value: object, *, context: str) -> bool:
    if not isinstance(value, bool):
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be a boolean")
    return value


def require_positive_int(value: object, *, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be an integer")
    if value <= 0:
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be greater than 0")
    return value


def extract_text_from_block(block: object, *, context: str) -> str:
    block_dict = require_object(block, context=context)
    block_type = require_string(block_dict.get("type"), context=f"{context}.type")
    if block_type in {"tool_result", "tool_use"}:
        raise RequestValidationError(
            501,
            "not_implemented_error",
            f"{context} type {block_type!r} is not supported yet",
        )
    if block_type != SUPPORTED_CONTENT_BLOCK_TYPE:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} type {block_type!r} is unsupported",
        )
    text = require_string(block_dict.get("text"), context=f"{context}.text")
    return text


def parse_message_content(content: object, *, context: str) -> str:
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} must be a string or a list of content blocks",
        )
    text_parts = []
    for index, block in enumerate(content):
        text_parts.append(extract_text_from_block(block, context=f"{context}[{index}]"))
    return "".join(text_parts)


def parse_anthropic_request(payload: object) -> dict:
    body = require_object(payload, context="request body")

    unknown_fields = sorted(set(body) - ALLOWED_REQUEST_FIELDS - UNSUPPORTED_REQUEST_FIELDS)
    if unknown_fields:
        field_list = ", ".join(repr(field) for field in unknown_fields)
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"Unsupported top-level field(s): {field_list}",
        )

    unsupported_fields = sorted(set(body) & UNSUPPORTED_REQUEST_FIELDS)
    if unsupported_fields:
        field_list = ", ".join(repr(field) for field in unsupported_fields)
        status_code = 501 if {"tool_choice", "tools", "thinking"} & set(unsupported_fields) else 400
        error_type = "not_implemented_error" if status_code == 501 else "invalid_request_error"
        raise RequestValidationError(
            status_code,
            error_type,
            f"Unsupported top-level field(s): {field_list}",
        )

    model = require_string(body.get("model"), context="model")
    if not model:
        raise RequestValidationError(400, "invalid_request_error", "model must not be empty")

    system = body.get("system")
    if system is not None:
        system = require_string(system, context="system")

    messages = body.get("messages")
    if not isinstance(messages, list):
        raise RequestValidationError(400, "invalid_request_error", "messages must be a list")
    if not messages:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            "messages must contain at least one message",
        )

    parsed_messages = []
    for index, message in enumerate(messages):
        message_dict = require_object(message, context=f"messages[{index}]")
        unknown_message_fields = sorted(set(message_dict) - {"content", "role"})
        if unknown_message_fields:
            field_list = ", ".join(repr(field) for field in unknown_message_fields)
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"Unsupported field(s) in messages[{index}]: {field_list}",
            )
        role = require_string(message_dict.get("role"), context=f"messages[{index}].role")
        if role not in SUPPORTED_MESSAGE_ROLES:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"messages[{index}].role must be one of {sorted(SUPPORTED_MESSAGE_ROLES)!r}",
            )
        content = parse_message_content(message_dict.get("content"), context=f"messages[{index}].content")
        parsed_messages.append({"role": role, "content": content})

    max_tokens = require_positive_int(body.get("max_tokens"), context="max_tokens")

    stream = body.get("stream", False)
    stream = require_bool(stream, context="stream")
    if stream:
        raise RequestValidationError(501, "not_implemented_error", "stream=true is not supported yet")

    return {
        "model": model,
        "system": system,
        "messages": parsed_messages,
        "max_tokens": max_tokens,
        "stream": stream,
    }


def build_upstream_request(parsed_request: dict) -> dict:
    messages = []
    system = parsed_request["system"]
    if system:
        messages.append({"role": "system", "content": system})
    messages.extend(parsed_request["messages"])
    return {
        "model": parsed_request["model"],
        "messages": messages,
        "max_tokens": parsed_request["max_tokens"],
        "stream": False,
    }


def decode_upstream_body(raw_body: bytes) -> str:
    return raw_body.decode("utf-8", errors="replace")


def forward_upstream(upstream_base_url: str, payload: dict) -> dict:
    upstream_url = f"{upstream_base_url}{UPSTREAM_MESSAGES_PATH}"
    upstream_request = request.Request(
        upstream_url,
        data=json_bytes(payload),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with request.urlopen(upstream_request, timeout=120) as response:
            raw_body = response.read()
    except error.HTTPError as exc:
        raw_body = exc.read()
        raise UpstreamRequestError(
            f"Upstream request failed with status {exc.code}",
            upstream_status=exc.code,
            upstream_body=decode_upstream_body(raw_body),
        ) from exc
    except error.URLError as exc:
        raise UpstreamRequestError(f"Could not reach upstream: {exc.reason}") from exc

    try:
        return json.loads(decode_upstream_body(raw_body))
    except json.JSONDecodeError as exc:
        raise UpstreamRequestError("Upstream returned invalid JSON") from exc


def map_stop_reason(finish_reason: object) -> str | None:
    if finish_reason == "stop":
        return "end_turn"
    if finish_reason == "length":
        return "max_tokens"
    return None


def build_anthropic_response(parsed_request: dict, upstream_response: object) -> dict:
    response_dict = require_object(upstream_response, context="upstream response")
    choices = response_dict.get("choices")
    if not isinstance(choices, list) or not choices:
        raise UpstreamRequestError("Upstream response did not include any choices")

    first_choice = require_object(choices[0], context="upstream response choices[0]")
    message = require_object(first_choice.get("message"), context="upstream response choices[0].message")
    content = message.get("content")
    if content is None:
        assistant_text = ""
    else:
        assistant_text = require_string(content, context="upstream response choices[0].message.content")

    usage = response_dict.get("usage")
    if usage is None:
        input_tokens = 0
        output_tokens = 0
    else:
        usage_dict = require_object(usage, context="upstream response usage")
        input_tokens = usage_dict.get("prompt_tokens", 0)
        output_tokens = usage_dict.get("completion_tokens", 0)
        if isinstance(input_tokens, bool) or not isinstance(input_tokens, int) or input_tokens < 0:
            raise UpstreamRequestError("Upstream response usage.prompt_tokens was invalid")
        if isinstance(output_tokens, bool) or not isinstance(output_tokens, int) or output_tokens < 0:
            raise UpstreamRequestError("Upstream response usage.completion_tokens was invalid")

    response_id = response_dict.get("id")
    if not isinstance(response_id, str) or not response_id:
        response_id = f"msg_{uuid.uuid4().hex}"

    return {
        "id": response_id,
        "type": "message",
        "role": "assistant",
        "model": parsed_request["model"],
        "content": [{"type": "text", "text": assistant_text}],
        "stop_reason": map_stop_reason(first_choice.get("finish_reason")),
        "stop_sequence": None,
        "usage": {
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
        },
    }


class ShimRequestHandler(BaseHTTPRequestHandler):
    server_version = "claude-gemma-shim/0.1"
    protocol_version = "HTTP/1.1"

    def _write_json(self, status_code: int, payload: dict) -> None:
        body = json_bytes(payload)
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("anthropic-version", ANTHROPIC_API_VERSION)
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
                "messages_url": f"{config['upstream_base_url']}{UPSTREAM_MESSAGES_PATH}",
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

    def _request_body_bytes(self) -> bytes:
        content_length_header = self.headers.get("Content-Length")
        if content_length_header is None:
            return b""
        try:
            content_length = int(content_length_header)
        except ValueError as exc:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                "Content-Length header must be an integer",
            ) from exc
        if content_length < 0:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                "Content-Length header must be non-negative",
            )
        return self.rfile.read(content_length)

    def _json_body(self) -> object:
        raw_body = self._request_body_bytes()
        if not raw_body:
            raise RequestValidationError(400, "invalid_request_error", "Request body must not be empty")
        try:
            return json.loads(raw_body.decode("utf-8"))
        except UnicodeDecodeError as exc:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                "Request body must be valid UTF-8 JSON",
            ) from exc
        except json.JSONDecodeError as exc:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"Request body was not valid JSON: {exc.msg}",
            ) from exc

    def _handle_messages(self) -> None:
        parsed_request = parse_anthropic_request(self._json_body())
        upstream_request = build_upstream_request(parsed_request)
        upstream_response = forward_upstream(self.server.shim_config["upstream_base_url"], upstream_request)
        response_payload = build_anthropic_response(parsed_request, upstream_response)
        self._write_json(200, response_payload)

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        path = urlparse(self.path).path
        if path == "/health":
            self._write_json(200, self._health_payload())
            return
        self._not_found()

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        path = urlparse(self.path).path
        try:
            if path == "/v1/messages":
                self._handle_messages()
                return
            self._not_found()
        except RequestValidationError as exc:
            self._write_json(exc.status_code, exc.to_payload())
        except UpstreamRequestError as exc:
            self._write_json(502, exc.to_payload())

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
