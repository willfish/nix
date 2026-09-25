#!/usr/bin/env python3
"""Stdio MCP server for TransformUK AWS access portal credentials."""

from __future__ import annotations

import json
import logging
import sys
import warnings

import portal
from constants import DEFAULT_REGION

warnings.filterwarnings("ignore")
logging.disable(logging.CRITICAL)

PROTOCOL = "2024-11-05"
INSTRUCTIONS = (
    "Use this server for TransformUK AWS IAM Identity Center credentials. "
    "Do not drive the Brave portal yourself and do not read sops secrets. "
    "Call export_credentials with the least-privileged account and role that "
    "can do the task. Source the returned env_file in the shell that needs "
    "AWS and do not read or print that file. Production administrator roles "
    "block on a local approval prompt; do not dismiss or answer it."
)

TOOLS = [
    {
        "name": "status",
        "description": (
            "Report whether the Brave portal session is signed in. "
            "Does not return secrets."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "login",
        "description": (
            "Sign in through a background Brave tab if the portal "
            "session is missing. Does not focus the tab. Closes the "
            "tab it opens. Does not return secrets."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "list_accounts",
        "description": (
            "List assigned AWS accounts. Signs in first if needed. "
            "Does not return credentials."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "list_roles",
        "description": (
            "List roles in one account. Name the account. "
            "Roles that need local approval before export are flagged."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "account_id": {"type": "string"},
                "account_name": {"type": "string"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "export_credentials",
        "description": (
            "Write short-lived credentials for one named role to a "
            "mode 0600 env file. Does not return secret values. "
            "Source env_file in the working shell. Production "
            "administrator roles wait for Will's local approval and "
            "fail closed if denied."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "account_id": {"type": "string"},
                "account_name": {"type": "string"},
                "role_name": {"type": "string"},
                "region": {"type": "string", "default": DEFAULT_REGION},
            },
            "required": ["role_name"],
            "additionalProperties": False,
        },
    },
]


def respond(message_id, result):
    sys.stdout.write(
        json.dumps({"jsonrpc": "2.0", "id": message_id, "result": result})
        + "\n"
    )
    sys.stdout.flush()


def fail(message_id, message):
    sys.stdout.write(
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": message_id,
                "error": {"code": -32603, "message": portal.redact(message)},
            }
        )
        + "\n"
    )
    sys.stdout.flush()


def tool_result(text, is_error=False):
    return {
        "content": [{"type": "text", "text": text.rstrip() + "\n"}],
        "isError": is_error,
    }


def format_status(payload):
    if not payload.get("logged_in"):
        return "Not signed in."
    expiry = payload.get("expires_at", "unknown")
    return f"Signed in until {expiry}."


def format_login(payload):
    if payload.get("opened_tab"):
        return "Signed in. Background tab closed."
    return "Already signed in."


def format_accounts(payload):
    lines = [
        f"{item['account_name']} {item['account_id']}"
        for item in payload.get("accounts") or []
    ]
    return "\n".join(lines) or "No accounts."


def format_roles(payload):
    gated = set(payload.get("production_admin_approval_required") or [])
    lines = [f"{payload.get('account_name')} {payload.get('account_id')}"]
    for role in payload.get("roles") or []:
        suffix = " (approval required)" if role in gated else ""
        lines.append(f"{role}{suffix}")
    return "\n".join(lines)


def format_export(payload):
    cached = " cached" if payload.get("cached") else ""
    return (
        f"{payload['account_name']} {payload['role_name']}{cached}\n"
        f"until {payload['expiration']}\n"
        f"{payload['source']}"
    )


def call_tool(name, arguments):
    arguments = arguments or {}
    client = portal.PortalClient()
    if name == "status":
        return tool_result(format_status(portal.session_status(client)))
    if name == "login":
        if portal.session_status(client).get("logged_in"):
            return tool_result(format_login({"opened_tab": False}))
        return tool_result(format_login(portal.login_if_needed(client)))
    if name == "list_accounts":
        return tool_result(
            format_accounts({"accounts": portal.list_accounts(client)})
        )
    if name == "list_roles":
        return tool_result(
            format_roles(
                portal.list_roles(
                    client,
                    account_id=arguments.get("account_id"),
                    account_name=arguments.get("account_name"),
                )
            )
        )
    if name == "export_credentials":
        return tool_result(
            format_export(
                portal.export_credentials(
                    client,
                    role_name=arguments.get("role_name"),
                    account_id=arguments.get("account_id"),
                    account_name=arguments.get("account_name"),
                    region=arguments.get("region") or DEFAULT_REGION,
                )
            )
        )
    raise portal.PortalError(f"unknown tool {name}")


def handle(message):
    method = message.get("method")
    message_id = message.get("id")
    if message_id is None:
        return
    if method == "initialize":
        respond(
            message_id,
            {
                "protocolVersion": PROTOCOL,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "aws-access-portal", "version": "1"},
                "instructions": INSTRUCTIONS,
            },
        )
        return
    if method == "ping":
        respond(message_id, {})
        return
    if method == "tools/list":
        respond(message_id, {"tools": TOOLS})
        return
    if method == "tools/call":
        params = message.get("params") or {}
        try:
            respond(
                message_id,
                call_tool(params.get("name"), params.get("arguments")),
            )
        except portal.PortalError as error:
            respond(message_id, tool_result(portal.redact(str(error)), True))
        except Exception as error:
            respond(
                message_id,
                tool_result(
                    portal.redact(str(error)) or "internal error",
                    True,
                ),
            )
        return
    fail(message_id, f"unsupported method {method}")


def main():
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        handle(message)


if __name__ == "__main__":
    main()
