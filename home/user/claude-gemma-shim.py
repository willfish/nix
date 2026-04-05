#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import re
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request
from urllib.parse import urlparse


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8090
DEFAULT_UPSTREAM_HOST = "127.0.0.1"
DEFAULT_UPSTREAM_PORT = 8080
UPSTREAM_MESSAGES_PATH = "/v1/chat/completions"
ALLOWED_REQUEST_FIELDS = {"max_tokens", "messages", "model", "stream", "system", "tools"}
UNSUPPORTED_REQUEST_FIELDS = {
    "metadata",
    "stop_sequences",
    "temperature",
    "thinking",
    "tool_choice",
    "top_k",
    "top_p",
}
SUPPORTED_MESSAGE_ROLES = {"assistant", "user"}
TOOL_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
SUPPORTED_TEXT_BLOCK_TYPE = "text"
SUPPORTED_SCHEMA_TYPES = {"array", "boolean", "integer", "number", "object", "string"}
SUPPORTED_SCHEMA_KEYS = {
    "additionalProperties",
    "description",
    "enum",
    "items",
    "properties",
    "required",
    "type",
}
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


class ClientDisconnectedError(Exception):
    pass


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


def require_string_list(value: object, *, context: str) -> list[str]:
    if not isinstance(value, list):
        raise RequestValidationError(400, "invalid_request_error", f"{context} must be a list")
    items = []
    for index, item in enumerate(value):
        items.append(require_string(item, context=f"{context}[{index}]"))
    return items


def reject_unknown_fields(value: dict, *, allowed_fields: set[str], context: str) -> None:
    unknown_fields = sorted(set(value) - allowed_fields)
    if unknown_fields:
        field_list = ", ".join(repr(field) for field in unknown_fields)
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"Unsupported field(s) in {context}: {field_list}",
        )


def require_tool_name(value: object, *, context: str) -> str:
    name = require_string(value, context=context)
    if not TOOL_NAME_PATTERN.fullmatch(name):
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} must match {TOOL_NAME_PATTERN.pattern!r}",
        )
    return name


def validate_schema_enum(value: object, *, context: str) -> None:
    if not isinstance(value, list) or not value:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} must be a non-empty list",
        )
    for index, item in enumerate(value):
        if isinstance(item, (dict, list)):
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}[{index}] must be a scalar JSON value",
            )


def validate_tool_input_schema(schema: object, *, context: str, top_level: bool = False) -> dict:
    schema_dict = require_object(schema, context=context)

    unsupported_keys = sorted(set(schema_dict) - SUPPORTED_SCHEMA_KEYS)
    if unsupported_keys:
        field_list = ", ".join(repr(field) for field in unsupported_keys)
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} uses unsupported JSON Schema keyword(s): {field_list}",
        )

    schema_type = require_string(schema_dict.get("type"), context=f"{context}.type")
    if schema_type not in SUPPORTED_SCHEMA_TYPES:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.type must be one of {sorted(SUPPORTED_SCHEMA_TYPES)!r}",
        )

    if top_level and schema_type != "object":
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.type must be 'object'",
        )

    description = schema_dict.get("description")
    if description is not None:
        require_string(description, context=f"{context}.description")

    enum_values = schema_dict.get("enum")
    if enum_values is not None:
        validate_schema_enum(enum_values, context=f"{context}.enum")

    if schema_type == "object":
        items = schema_dict.get("items")
        if items is not None:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}.items is only supported when {context}.type is 'array'",
            )

        properties = schema_dict.get("properties", {})
        if not isinstance(properties, dict):
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}.properties must be an object",
            )

        required_names = schema_dict.get("required", [])
        required_names = require_string_list(required_names, context=f"{context}.required")

        additional_properties = schema_dict.get("additionalProperties")
        if additional_properties is not None and not isinstance(additional_properties, bool):
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}.additionalProperties must be a boolean",
            )

        property_names = set()
        for property_name, property_schema in properties.items():
            if not isinstance(property_name, str):
                raise RequestValidationError(
                    400,
                    "invalid_request_error",
                    f"{context}.properties keys must be strings",
                )
            property_names.add(property_name)
            validate_tool_input_schema(
                property_schema,
                context=f"{context}.properties[{property_name!r}]",
            )

        missing_required = [name for name in required_names if name not in property_names]
        if missing_required:
            missing_list = ", ".join(repr(name) for name in missing_required)
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}.required references undefined propertie(s): {missing_list}",
            )
    else:
        if "properties" in schema_dict or "required" in schema_dict or "additionalProperties" in schema_dict:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context} only supports properties/required/additionalProperties for object schemas",
            )

    if schema_type == "array":
        if "items" not in schema_dict:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}.items is required when {context}.type is 'array'",
            )
        validate_tool_input_schema(schema_dict["items"], context=f"{context}.items")
    elif "items" in schema_dict:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.items is only supported when {context}.type is 'array'",
        )

    return schema_dict


def parse_tool_definition(tool: object, *, context: str) -> dict:
    tool_dict = require_object(tool, context=context)
    reject_unknown_fields(tool_dict, allowed_fields={"description", "input_schema", "name"}, context=context)

    name = require_tool_name(tool_dict.get("name"), context=f"{context}.name")
    input_schema = validate_tool_input_schema(
        tool_dict.get("input_schema"),
        context=f"{context}.input_schema",
        top_level=True,
    )

    function_definition = {
        "name": name,
        "parameters": input_schema,
    }
    if "description" in tool_dict:
        function_definition["description"] = require_string(
            tool_dict.get("description"),
            context=f"{context}.description",
        )

    return {
        "type": "function",
        "function": function_definition,
    }


def extract_text_from_block(block: object, *, context: str) -> str:
    block_dict = require_object(block, context=context)
    block_type = require_string(block_dict.get("type"), context=f"{context}.type")
    if block_type != SUPPORTED_TEXT_BLOCK_TYPE:
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} type {block_type!r} is unsupported",
        )
    text = require_string(block_dict.get("text"), context=f"{context}.text")
    return text


def parse_tool_use_block(block: object, *, context: str) -> tuple[dict, str]:
    block_dict = require_object(block, context=context)
    reject_unknown_fields(block_dict, allowed_fields={"id", "input", "name", "type"}, context=context)

    block_type = require_string(block_dict.get("type"), context=f"{context}.type")
    if block_type != "tool_use":
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.type must be 'tool_use'",
        )

    tool_use_id = require_string(block_dict.get("id"), context=f"{context}.id")
    tool_name = require_tool_name(block_dict.get("name"), context=f"{context}.name")
    tool_input = require_object(block_dict.get("input"), context=f"{context}.input")

    return (
        {
            "id": tool_use_id,
            "type": "function",
            "function": {
                "name": tool_name,
                "arguments": json.dumps(tool_input, separators=(",", ":"), sort_keys=True),
            },
        },
        tool_use_id,
    )


def parse_tool_result_content(content: object, *, context: str) -> str:
    if content is None:
        return ""
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


def parse_assistant_message_content(content: object, *, context: str) -> tuple[dict, list[str] | None]:
    if isinstance(content, str):
        return {"role": "assistant", "content": content}, None
    if not isinstance(content, list):
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} must be a string or a list of content blocks",
        )

    text_parts = []
    tool_calls = []
    tool_use_ids = []
    saw_tool_use = False
    for index, block in enumerate(content):
        block_dict = require_object(block, context=f"{context}[{index}]")
        block_type = require_string(block_dict.get("type"), context=f"{context}[{index}].type")
        if block_type == SUPPORTED_TEXT_BLOCK_TYPE:
            if saw_tool_use:
                raise RequestValidationError(
                    400,
                    "invalid_request_error",
                    f"{context}[{index}] text after tool_use is unsupported",
                )
            text_parts.append(extract_text_from_block(block_dict, context=f"{context}[{index}]"))
            continue
        if block_type != "tool_use":
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}[{index}] type {block_type!r} is unsupported for assistant messages",
            )
        saw_tool_use = True
        tool_call, tool_use_id = parse_tool_use_block(block_dict, context=f"{context}[{index}]")
        if tool_use_id in tool_use_ids:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}[{index}].id {tool_use_id!r} is duplicated",
            )
        tool_use_ids.append(tool_use_id)
        tool_calls.append(tool_call)

    message = {"role": "assistant", "content": "".join(text_parts)}
    if tool_calls:
        message["tool_calls"] = tool_calls
    return message, tool_use_ids or None


def parse_tool_result_block(
    block: object,
    *,
    context: str,
    expected_tool_use_ids: set[str],
) -> tuple[dict, str]:
    block_dict = require_object(block, context=context)
    reject_unknown_fields(block_dict, allowed_fields={"content", "is_error", "tool_use_id", "type"}, context=context)

    block_type = require_string(block_dict.get("type"), context=f"{context}.type")
    if block_type != "tool_result":
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.type must be 'tool_result'",
        )

    tool_use_id = require_string(block_dict.get("tool_use_id"), context=f"{context}.tool_use_id")
    if tool_use_id not in expected_tool_use_ids:
        expected_list = ", ".join(repr(item) for item in sorted(expected_tool_use_ids))
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context}.tool_use_id {tool_use_id!r} did not match the pending tool_use id(s): {expected_list}",
        )

    tool_message = {
        "role": "tool",
        "tool_call_id": tool_use_id,
        "content": parse_tool_result_content(block_dict.get("content"), context=f"{context}.content"),
    }
    if "is_error" in block_dict:
        tool_message["is_error"] = require_bool(block_dict.get("is_error"), context=f"{context}.is_error")

    return (tool_message, tool_use_id)


def parse_user_message_content(
    content: object,
    *,
    context: str,
    pending_tool_use_ids: list[str] | None,
) -> tuple[list[dict], bool]:
    if isinstance(content, str):
        if pending_tool_use_ids:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context} must start with tool_result blocks for the pending tool_use id(s): "
                f"{', '.join(repr(item) for item in pending_tool_use_ids)}",
            )
        return [{"role": "user", "content": content}], False

    if not isinstance(content, list):
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"{context} must be a string or a list of content blocks",
        )

    tool_messages = []
    seen_tool_result_ids = []
    text_parts = []
    saw_text = False
    expected_tool_use_ids = set(pending_tool_use_ids or [])

    for index, block in enumerate(content):
        block_dict = require_object(block, context=f"{context}[{index}]")
        block_type = require_string(block_dict.get("type"), context=f"{context}[{index}].type")
        if block_type == "tool_result":
            if saw_text:
                raise RequestValidationError(
                    400,
                    "invalid_request_error",
                    f"{context}[{index}] tool_result blocks must come before any text blocks",
                )
            if not pending_tool_use_ids:
                raise RequestValidationError(
                    400,
                    "invalid_request_error",
                    f"{context}[{index}] tool_result is only valid immediately after an assistant tool_use message",
                )
            tool_message, tool_use_id = parse_tool_result_block(
                block_dict,
                context=f"{context}[{index}]",
                expected_tool_use_ids=expected_tool_use_ids,
            )
            if tool_use_id in seen_tool_result_ids:
                raise RequestValidationError(
                    400,
                    "invalid_request_error",
                    f"{context}[{index}].tool_use_id {tool_use_id!r} is duplicated",
                )
            seen_tool_result_ids.append(tool_use_id)
            tool_messages.append(tool_message)
            continue

        if block_type != SUPPORTED_TEXT_BLOCK_TYPE:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context}[{index}] type {block_type!r} is unsupported for user messages",
            )

        saw_text = True
        text_parts.append(extract_text_from_block(block_dict, context=f"{context}[{index}]"))

    if pending_tool_use_ids:
        if not seen_tool_result_ids:
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context} must start with tool_result blocks for the pending tool_use id(s): "
                f"{', '.join(repr(item) for item in pending_tool_use_ids)}",
            )
        missing_ids = [tool_use_id for tool_use_id in pending_tool_use_ids if tool_use_id not in seen_tool_result_ids]
        if missing_ids:
            missing_list = ", ".join(repr(item) for item in missing_ids)
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"{context} did not include tool_result blocks for pending tool_use id(s): {missing_list}",
            )

    messages = list(tool_messages)
    user_text = "".join(text_parts)
    if not tool_messages or user_text:
        messages.append({"role": "user", "content": user_text})
    return messages, bool(tool_messages)


def parse_message_content(content: object, *, context: str, role: str, pending_tool_use_ids: list[str] | None) -> tuple[list[dict], list[str] | None]:
    if role == "assistant":
        message, next_pending_tool_use_ids = parse_assistant_message_content(content, context=context)
        return [message], next_pending_tool_use_ids

    messages, consumed_tool_results = parse_user_message_content(
        content,
        context=context,
        pending_tool_use_ids=pending_tool_use_ids,
    )
    return messages, None if consumed_tool_results else pending_tool_use_ids


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
        status_code = 501 if {"thinking", "tool_choice"} & set(unsupported_fields) else 400
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

    tools = body.get("tools")
    if tools is None:
        parsed_tools = None
    else:
        if not isinstance(tools, list):
            raise RequestValidationError(400, "invalid_request_error", "tools must be a list")
        parsed_tools = [
            parse_tool_definition(tool, context=f"tools[{index}]")
            for index, tool in enumerate(tools)
        ]

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
    pending_tool_use_ids = None
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
        if pending_tool_use_ids and role != "user":
            pending_list = ", ".join(repr(item) for item in pending_tool_use_ids)
            raise RequestValidationError(
                400,
                "invalid_request_error",
                f"messages[{index}].role must be 'user' to provide tool_result blocks for pending tool_use id(s): {pending_list}",
            )
        content_messages, pending_tool_use_ids = parse_message_content(
            message_dict.get("content"),
            context=f"messages[{index}].content",
            role=role,
            pending_tool_use_ids=pending_tool_use_ids,
        )
        parsed_messages.extend(content_messages)

    if pending_tool_use_ids:
        pending_list = ", ".join(repr(item) for item in pending_tool_use_ids)
        raise RequestValidationError(
            400,
            "invalid_request_error",
            f"messages must include a user tool_result message immediately after assistant tool_use id(s): {pending_list}",
        )

    max_tokens = require_positive_int(body.get("max_tokens"), context="max_tokens")

    stream = body.get("stream", False)
    stream = require_bool(stream, context="stream")

    return {
        "model": model,
        "system": system,
        "messages": parsed_messages,
        "max_tokens": max_tokens,
        "stream": stream,
        "tools": parsed_tools,
    }


def build_upstream_request(parsed_request: dict) -> dict:
    messages = []
    system = parsed_request["system"]
    if system:
        messages.append({"role": "system", "content": system})
    messages.extend(parsed_request["messages"])
    upstream_request = {
        "model": parsed_request["model"],
        "messages": messages,
        "max_tokens": parsed_request["max_tokens"],
        "stream": parsed_request["stream"],
    }
    if parsed_request["tools"]:
        upstream_request["tools"] = parsed_request["tools"]
    return upstream_request


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


def open_upstream_stream(upstream_base_url: str, payload: dict):
    upstream_url = f"{upstream_base_url}{UPSTREAM_MESSAGES_PATH}"
    upstream_request = request.Request(
        upstream_url,
        data=json_bytes(payload),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        return request.urlopen(upstream_request, timeout=120)
    except error.HTTPError as exc:
        raw_body = exc.read()
        raise UpstreamRequestError(
            f"Upstream request failed with status {exc.code}",
            upstream_status=exc.code,
            upstream_body=decode_upstream_body(raw_body),
        ) from exc
    except error.URLError as exc:
        raise UpstreamRequestError(f"Could not reach upstream: {exc.reason}") from exc


def iter_sse_data_events(response) -> object:
    data_lines = []
    while True:
        raw_line = response.readline()
        if not raw_line:
            if data_lines:
                yield "\n".join(data_lines)
            return

        line = raw_line.decode("utf-8", errors="replace")
        if line in ("\n", "\r\n"):
            if data_lines:
                yield "\n".join(data_lines)
                data_lines = []
            continue

        if not line.startswith("data:"):
            continue

        data = line[5:]
        if data.startswith(" "):
            data = data[1:]
        data_lines.append(data.rstrip("\r\n"))


def require_non_negative_int(value: object, *, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise UpstreamRequestError(f"{context} was invalid")
    if value < 0:
        raise UpstreamRequestError(f"{context} was invalid")
    return value


def extract_stream_usage(response_dict: dict) -> tuple[int, int]:
    usage = response_dict.get("usage")
    if usage is not None:
        usage_dict = require_object(usage, context="upstream stream usage")
        input_tokens = require_non_negative_int(
            usage_dict.get("prompt_tokens", 0),
            context="upstream stream usage.prompt_tokens",
        )
        output_tokens = require_non_negative_int(
            usage_dict.get("completion_tokens", 0),
            context="upstream stream usage.completion_tokens",
        )
        return input_tokens, output_tokens

    timings = response_dict.get("timings")
    if timings is None:
        return 0, 0

    timings_dict = require_object(timings, context="upstream stream timings")
    input_tokens = require_non_negative_int(
        timings_dict.get("prompt_n", 0),
        context="upstream stream timings.prompt_n",
    )
    output_tokens = require_non_negative_int(
        timings_dict.get("predicted_n", 0),
        context="upstream stream timings.predicted_n",
    )
    return input_tokens, output_tokens


def map_stop_reason(finish_reason: object) -> str | None:
    if finish_reason == "tool_calls":
        return "tool_use"
    if finish_reason == "length":
        return "max_tokens"
    return "end_turn"


def build_anthropic_tool_use_block(tool_call: object, *, context: str) -> dict:
    tool_call_dict = require_object(tool_call, context=context)
    reject_unknown_fields(tool_call_dict, allowed_fields={"function", "id", "type"}, context=context)

    tool_call_type = require_string(tool_call_dict.get("type"), context=f"{context}.type")
    if tool_call_type != "function":
        raise UpstreamRequestError(f"{context}.type {tool_call_type!r} is unsupported")

    tool_call_id = require_string(tool_call_dict.get("id"), context=f"{context}.id")
    function_dict = require_object(tool_call_dict.get("function"), context=f"{context}.function")
    reject_unknown_fields(
        function_dict,
        allowed_fields={"arguments", "name"},
        context=f"{context}.function",
    )

    function_name = require_tool_name(function_dict.get("name"), context=f"{context}.function.name")
    raw_arguments = require_string(function_dict.get("arguments"), context=f"{context}.function.arguments")
    try:
        tool_input = json.loads(raw_arguments)
    except json.JSONDecodeError as exc:
        raise UpstreamRequestError(f"{context}.function.arguments was not valid JSON") from exc
    tool_input = require_object(tool_input, context=f"{context}.function.arguments")

    return {
        "type": "tool_use",
        "id": tool_call_id,
        "name": function_name,
        "input": tool_input,
    }


def build_anthropic_response(parsed_request: dict, upstream_response: object) -> dict:
    try:
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

        tool_calls = message.get("tool_calls")
        anthropic_content = []
        if assistant_text:
            anthropic_content.append({"type": "text", "text": assistant_text})
        if tool_calls is not None:
            if not isinstance(tool_calls, list) or not tool_calls:
                raise UpstreamRequestError("Upstream response message.tool_calls was invalid")
            for index, tool_call in enumerate(tool_calls):
                anthropic_content.append(
                    build_anthropic_tool_use_block(
                        tool_call,
                        context=f"upstream response choices[0].message.tool_calls[{index}]",
                    )
                )
        if not anthropic_content:
            anthropic_content.append({"type": "text", "text": assistant_text})

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
            "content": anthropic_content,
            "stop_reason": map_stop_reason(first_choice.get("finish_reason")),
            "stop_sequence": None,
            "usage": {
                "input_tokens": input_tokens,
                "output_tokens": output_tokens,
            },
        }
    except RequestValidationError as exc:
        raise UpstreamRequestError(exc.message) from exc


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

    def _begin_sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.send_header("anthropic-version", ANTHROPIC_API_VERSION)
        self.end_headers()
        self.close_connection = True

    def _write_sse_event(self, event_type: str, payload: dict) -> None:
        event_bytes = (
            f"event: {event_type}\n"
            f"data: {json.dumps(payload, sort_keys=True, separators=(',', ':'))}\n\n"
        ).encode("utf-8")
        try:
            self.wfile.write(event_bytes)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError) as exc:
            raise ClientDisconnectedError() from exc

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

    def _drain_request_body(self) -> None:
        self._request_body_bytes()

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
        if parsed_request["stream"]:
            self._handle_streaming_messages(parsed_request, upstream_request)
            return
        upstream_response = forward_upstream(self.server.shim_config["upstream_base_url"], upstream_request)
        response_payload = build_anthropic_response(parsed_request, upstream_response)
        self._write_json(200, response_payload)

    def _handle_streaming_messages(self, parsed_request: dict, upstream_request: dict) -> None:
        self._begin_sse()

        try:
            with open_upstream_stream(self.server.shim_config["upstream_base_url"], upstream_request) as upstream_response:
                stream_state = {
                    "message_started": False,
                    "response_id": None,
                    "response_model": parsed_request["model"],
                    "next_content_index": 0,
                    "text_block_index": None,
                    "tool_blocks": {},
                }

                for raw_event in iter_sse_data_events(upstream_response):
                    if raw_event == "[DONE]":
                        break

                    try:
                        chunk = json.loads(raw_event)
                    except json.JSONDecodeError as exc:
                        raise UpstreamRequestError("Upstream stream returned invalid JSON") from exc

                    self._relay_upstream_stream_chunk(parsed_request, chunk, stream_state)

                if stream_state["message_started"]:
                    self._finish_streaming_message(stream_state, stop_reason="end_turn", output_tokens=0)
        except ClientDisconnectedError:
            return
        except UpstreamRequestError as exc:
            self._write_sse_event("error", exc.to_payload())

    def _start_streaming_message(self, parsed_request: dict, response_dict: dict, stream_state: dict) -> None:
        response_id = response_dict.get("id")
        if not isinstance(response_id, str) or not response_id:
            response_id = f"msg_{uuid.uuid4().hex}"

        response_model = response_dict.get("model")
        if not isinstance(response_model, str) or not response_model:
            response_model = parsed_request["model"]

        stream_state["message_started"] = True
        stream_state["response_id"] = response_id
        stream_state["response_model"] = response_model
        self._write_sse_event(
            "message_start",
            {
                "type": "message_start",
                "message": {
                    "id": response_id,
                    "type": "message",
                    "role": "assistant",
                    "model": response_model,
                    "content": [],
                    "stop_reason": None,
                    "stop_sequence": None,
                    "usage": {
                        "input_tokens": 0,
                        "output_tokens": 0,
                    },
                },
            },
        )

    def _start_text_block(self, stream_state: dict) -> int:
        if stream_state["text_block_index"] is not None:
            return stream_state["text_block_index"]

        content_index = stream_state["next_content_index"]
        stream_state["next_content_index"] += 1
        stream_state["text_block_index"] = content_index
        self._write_sse_event(
            "content_block_start",
            {
                "type": "content_block_start",
                "index": content_index,
                "content_block": {
                    "type": "text",
                    "text": "",
                },
            },
        )
        return content_index

    def _stop_text_block(self, stream_state: dict) -> None:
        content_index = stream_state["text_block_index"]
        if content_index is None:
            return
        self._write_sse_event(
            "content_block_stop",
            {
                "type": "content_block_stop",
                "index": content_index,
            },
        )
        stream_state["text_block_index"] = None

    def _tool_block_state(self, tool_delta: dict, stream_state: dict, *, context: str) -> dict:
        upstream_tool_index = require_non_negative_int(
            tool_delta.get("index"),
            context=f"{context}.index",
        )

        tool_block = stream_state["tool_blocks"].get(upstream_tool_index)
        if tool_block is not None:
            tool_call_id = tool_delta.get("id")
            if tool_call_id is not None and require_string(tool_call_id, context=f"{context}.id") != tool_block["id"]:
                raise UpstreamRequestError(f"{context}.id changed within the same tool call")
            if "type" in tool_delta:
                tool_call_type = require_string(tool_delta.get("type"), context=f"{context}.type")
                if tool_call_type != "function":
                    raise UpstreamRequestError(f"{context}.type {tool_call_type!r} is unsupported")

            function = tool_delta.get("function")
            if function is not None:
                function_dict = require_object(function, context=f"{context}.function")
                tool_name = function_dict.get("name")
                if tool_name is not None and require_tool_name(tool_name, context=f"{context}.function.name") != tool_block["name"]:
                    raise UpstreamRequestError(f"{context}.function.name changed within the same tool call")
            return tool_block

        tool_call_id = require_string(tool_delta.get("id"), context=f"{context}.id")
        tool_call_type = require_string(tool_delta.get("type"), context=f"{context}.type")
        if tool_call_type != "function":
            raise UpstreamRequestError(f"{context}.type {tool_call_type!r} is unsupported")

        function = require_object(tool_delta.get("function"), context=f"{context}.function")
        tool_name = require_tool_name(function.get("name"), context=f"{context}.function.name")

        content_index = stream_state["next_content_index"]
        stream_state["next_content_index"] += 1
        tool_block = {
            "content_index": content_index,
            "id": tool_call_id,
            "name": tool_name,
        }
        stream_state["tool_blocks"][upstream_tool_index] = tool_block
        self._write_sse_event(
            "content_block_start",
            {
                "type": "content_block_start",
                "index": content_index,
                "content_block": {
                    "type": "tool_use",
                    "id": tool_call_id,
                    "name": tool_name,
                    "input": {},
                },
            },
        )
        return tool_block

    def _stop_tool_blocks(self, stream_state: dict) -> None:
        for upstream_tool_index in sorted(stream_state["tool_blocks"]):
            tool_block = stream_state["tool_blocks"][upstream_tool_index]
            self._write_sse_event(
                "content_block_stop",
                {
                    "type": "content_block_stop",
                    "index": tool_block["content_index"],
                },
            )
        stream_state["tool_blocks"].clear()

    def _finish_streaming_message(self, stream_state: dict, *, stop_reason: str, output_tokens: int) -> None:
        self._stop_text_block(stream_state)
        self._stop_tool_blocks(stream_state)
        self._write_sse_event(
            "message_delta",
            {
                "type": "message_delta",
                "delta": {
                    "stop_reason": stop_reason,
                    "stop_sequence": None,
                },
                "usage": {
                    "output_tokens": output_tokens,
                },
            },
        )
        self._write_sse_event(
            "message_stop",
            {
                "type": "message_stop",
            },
        )
        stream_state["message_started"] = False

    def _relay_upstream_stream_chunk(self, parsed_request: dict, chunk: object, stream_state: dict) -> None:
        response_dict = require_object(chunk, context="upstream stream chunk")
        if not stream_state["message_started"]:
            self._start_streaming_message(parsed_request, response_dict, stream_state)

        choices = response_dict.get("choices")
        if not isinstance(choices, list) or not choices:
            raise UpstreamRequestError("Upstream stream chunk did not include any choices")

        first_choice = require_object(choices[0], context="upstream stream chunk choices[0]")
        delta = first_choice.get("delta", {})
        delta_dict = require_object(delta, context="upstream stream chunk choices[0].delta")

        delta_content = delta_dict.get("content")
        if delta_content is not None:
            if stream_state["tool_blocks"]:
                raise UpstreamRequestError("Upstream stream emitted text after tool_use deltas, which is unsupported")
            delta_text = require_string(
                delta_content,
                context="upstream stream chunk choices[0].delta.content",
            )
            if delta_text:
                text_block_index = self._start_text_block(stream_state)
                self._write_sse_event(
                    "content_block_delta",
                    {
                        "type": "content_block_delta",
                        "index": text_block_index,
                        "delta": {
                            "type": "text_delta",
                            "text": delta_text,
                        },
                    },
                )

        tool_calls = delta_dict.get("tool_calls")
        if tool_calls is not None:
            if not isinstance(tool_calls, list) or not tool_calls:
                raise UpstreamRequestError("Upstream stream chunk choices[0].delta.tool_calls was invalid")
            self._stop_text_block(stream_state)
            for index, tool_delta in enumerate(tool_calls):
                tool_delta_dict = require_object(
                    tool_delta,
                    context=f"upstream stream chunk choices[0].delta.tool_calls[{index}]",
                )
                tool_block = self._tool_block_state(
                    tool_delta_dict,
                    stream_state,
                    context=f"upstream stream chunk choices[0].delta.tool_calls[{index}]",
                )
                function = tool_delta_dict.get("function")
                if function is None:
                    continue
                function_dict = require_object(
                    function,
                    context=f"upstream stream chunk choices[0].delta.tool_calls[{index}].function",
                )
                partial_json = function_dict.get("arguments")
                if partial_json is None:
                    continue
                partial_json = require_string(
                    partial_json,
                    context=f"upstream stream chunk choices[0].delta.tool_calls[{index}].function.arguments",
                )
                if not partial_json:
                    continue
                self._write_sse_event(
                    "content_block_delta",
                    {
                        "type": "content_block_delta",
                        "index": tool_block["content_index"],
                        "delta": {
                            "type": "input_json_delta",
                            "partial_json": partial_json,
                        },
                    },
                )

        finish_reason = first_choice.get("finish_reason")
        if finish_reason is None:
            return

        _, output_tokens = extract_stream_usage(response_dict)
        self._finish_streaming_message(
            stream_state,
            stop_reason=map_stop_reason(finish_reason),
            output_tokens=output_tokens,
        )

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
            self._drain_request_body()
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
