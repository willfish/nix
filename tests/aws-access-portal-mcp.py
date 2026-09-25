"""Offline tests for the AWS access portal credential MCP."""

import json
import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "home/config/llm/scripts/aws-access-portal"
sys.path.insert(0, str(PACKAGE))

import portal  # noqa: E402
import server  # noqa: E402
from constants import PRODUCTION_ACCOUNT_ID  # noqa: E402


SECRET = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
TOKEN = "IQoJb3JpZ2luX2VjE" + "A" * 40


class FakePortal:
    def __init__(self):
        self.token = "portal-session"
        self.valid = True
        self.calls = []
        self.opened = False
        self.accounts = [
            {
                "accountId": "844815912454",
                "accountName": "Development",
                "emailAddress": "dev@example.com",
            },
            {
                "accountId": PRODUCTION_ACCOUNT_ID,
                "accountName": "Production",
                "emailAddress": "prod@example.com",
            },
        ]
        self.roles = {
            "844815912454": [
                "TariffReadOnlyAccess",
                "TariffAdministratorAccess",
            ],
            PRODUCTION_ACCOUNT_ID: [
                "TariffReadOnlyAccess",
                "TariffAdministratorAccess",
            ],
        }

    def auth_token(self):
        return self.token

    def api(self, token, path, query=None):
        self.calls.append((path, query or {}))
        if path == "/token/whoAmI":
            if not self.valid:
                raise portal.PortalError(
                    "portal /token/whoAmI returned HTTP 401"
                )
            return {
                "directoryId": "d-9c677042e2",
                "expireDate": 9_000_000_000_000,
            }
        if path == "/assignment/accounts":
            return {"accountList": self.accounts, "nextToken": None}
        if path == "/assignment/roles":
            account_id = (query or {}).get("account_id")
            return {
                "roleList": [
                    {"roleName": name, "accountId": account_id}
                    for name in self.roles[account_id]
                ],
                "nextToken": None,
            }
        if path == "/federation/credentials":
            return {
                "roleCredentials": {
                    "accessKeyId": "ASIAIOSFODNN7EXAMPLE",
                    "secretAccessKey": SECRET,
                    "sessionToken": TOKEN,
                    "expiration": 9_000_000_000_000,
                }
            }
        raise portal.PortalError(f"unexpected {path}")

    def open_background_tab(self):
        self.opened = True
        raise AssertionError("authenticated export must not open a tab")


class ServerTests(unittest.TestCase):
    def test_tool_text_is_plain(self):
        text = server.format_export(
            {
                "account_name": "Development",
                "role_name": "TariffReadOnlyAccess",
                "expiration": "2026-09-25T19:32:54Z",
                "source": "source /tmp/role.env",
                "cached": False,
            }
        )
        self.assertNotIn("{", text)
        self.assertIn("source /tmp/role.env", text)
        self.assertEqual(
            server.format_status({"logged_in": True, "expires_at": "16:50Z"}),
            "Signed in until 16:50Z.",
        )

    def test_initialize_and_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env["XDG_RUNTIME_DIR"] = directory
            process = subprocess.run(
                [sys.executable, str(PACKAGE / "server.py")],
                input="\n".join(
                    [
                        json.dumps(
                            {
                                "jsonrpc": "2.0",
                                "id": 1,
                                "method": "initialize",
                                "params": {"protocolVersion": "2024-11-05"},
                            }
                        ),
                        json.dumps(
                            {
                                "jsonrpc": "2.0",
                                "method": "notifications/initialized",
                            }
                        ),
                        json.dumps(
                            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}
                        ),
                    ]
                )
                + "\n",
                text=True,
                capture_output=True,
                cwd=directory,
                env=env,
                check=False,
            )
        self.assertEqual(process.returncode, 0, process.stderr)
        messages = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual(
            messages[0]["result"]["serverInfo"]["name"], "aws-access-portal"
        )
        names = [tool["name"] for tool in messages[1]["result"]["tools"]]
        self.assertEqual(
            names,
            [
                "status",
                "login",
                "list_accounts",
                "list_roles",
                "export_credentials",
            ],
        )
        self.assertIn("local approval", messages[0]["result"]["instructions"])

    def test_non_admin_export_writes_env_without_returning_secrets(self):
        client = FakePortal()
        with tempfile.TemporaryDirectory() as directory:
            os.environ["XDG_RUNTIME_DIR"] = directory
            result = portal.export_credentials(
                client,
                role_name="TariffReadOnlyAccess",
                account_name="Development",
                approver=lambda *args: self.fail(
                    "approval should not be requested"
                ),
            )
            text = Path(result["env_file"]).read_text()
            mode = stat.S_IMODE(os.stat(result["env_file"]).st_mode)
        self.assertEqual(mode, 0o600)
        self.assertNotIn(SECRET, json.dumps(result))
        self.assertNotIn(TOKEN, json.dumps(result))
        self.assertIn(SECRET, text)
        self.assertIn("unset AWS_PROFILE", text)
        self.assertFalse(client.opened)
        self.assertNotIn(
            "/federation/credentials",
            [path for path, _query in client.calls[:1]],
        )
        self.assertIn(
            "/federation/credentials",
            [path for path, _query in client.calls],
        )

    def test_cached_non_admin_export_does_not_fetch_again(self):
        client = FakePortal()
        with tempfile.TemporaryDirectory() as directory:
            os.environ["XDG_RUNTIME_DIR"] = directory
            portal.export_credentials(
                client,
                role_name="TariffReadOnlyAccess",
                account_id="844815912454",
            )
            client.calls.clear()
            result = portal.export_credentials(
                client,
                role_name="TariffReadOnlyAccess",
                account_id="844815912454",
            )
        self.assertTrue(result["cached"])
        self.assertNotIn(
            "/federation/credentials",
            [path for path, _query in client.calls],
        )

    def test_production_admin_requires_approval_before_fetch(self):
        client = FakePortal()
        seen = []

        def deny(*args):
            seen.append(args)
            return False

        with tempfile.TemporaryDirectory() as directory:
            os.environ["XDG_RUNTIME_DIR"] = directory
            with self.assertRaises(portal.PortalError) as raised:
                portal.export_credentials(
                    client,
                    role_name="TariffAdministratorAccess",
                    account_name="Production",
                    approver=deny,
                )
        self.assertIn("not approved", str(raised.exception))
        self.assertEqual(seen[0][1], PRODUCTION_ACCOUNT_ID)
        self.assertNotIn(
            "/federation/credentials",
            [path for path, _query in client.calls],
        )

    def test_production_admin_fetches_only_after_approval(self):
        client = FakePortal()
        with tempfile.TemporaryDirectory() as directory:
            os.environ["XDG_RUNTIME_DIR"] = directory
            result = portal.export_credentials(
                client,
                role_name="TariffAdministratorAccess",
                account_id=PRODUCTION_ACCOUNT_ID,
                approver=lambda *args: True,
            )
        self.assertEqual(result["account_id"], PRODUCTION_ACCOUNT_ID)
        self.assertNotIn(SECRET, json.dumps(result))
        self.assertIn(
            "/federation/credentials",
            [path for path, _query in client.calls],
        )

    def test_development_admin_does_not_require_approval(self):
        client = FakePortal()
        with tempfile.TemporaryDirectory() as directory:
            os.environ["XDG_RUNTIME_DIR"] = directory
            portal.export_credentials(
                client,
                role_name="TariffAdministratorAccess",
                account_name="Development",
                approver=lambda *args: self.fail(
                    "only production admin is gated"
                ),
            )

    def test_login_uses_background_tab_and_closes_it(self):
        events = []

        class Page:
            def navigate(self, url):
                events.append(("navigate", url))

            def evaluate(self, expression):
                events.append("evaluate")
                return {
                    "href": (
                        "https://d-9c677042e2.awsapps.com"
                        "/start/#/?tab=accounts"
                    ),
                    "username": False,
                    "password": False,
                    "mfa": False,
                    "accounts": True,
                    "alert": "",
                    "focused": False,
                }

        class Tab:
            def __enter__(self):
                events.append("open-background")
                return Page()

            def __exit__(self, exc_type, exc, tb):
                events.append("close")
                return False

        class Client(FakePortal):
            def __init__(self):
                super().__init__()
                self.token = None
                self.valid = False
                self.after_login = False

            def auth_token(self):
                return "portal-session" if self.after_login else None

            def open_background_tab(self):
                self.after_login = True
                self.valid = True
                return Tab()

            def page_state(self, page):
                return page.evaluate("")

        client = Client()
        result = portal.login_if_needed(client, sleeper=lambda _seconds: None)
        self.assertTrue(result["opened_tab"])
        self.assertEqual(events[0], "open-background")
        self.assertEqual(events[-1], "close")
        self.assertNotIn("Target.activateTarget", json.dumps(events))

    def test_focus_methods_are_refused(self):
        socket = portal.CdpSocket.__new__(portal.CdpSocket)
        with self.assertRaises(portal.PortalError):
            socket.call("Page.bringToFront")
        with self.assertRaises(portal.PortalError):
            socket.call("Target.activateTarget")

    def test_approval_hook_cannot_be_used_unless_explicitly_enabled(self):
        env = os.environ.copy()
        env.pop("AWS_PORTAL_ALLOW_APPROVAL_HOOK", None)
        env["AWS_PORTAL_APPROVAL_COMMAND"] = "printf approve"
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        script = textwrap.dedent(
            """
            import os, sys
            sys.path.insert(0, os.environ["PACKAGE"])
            import portal
            try:
                portal.request_approval(
                    "Production",
                    "382373577178",
                    "TariffAdministratorAccess",
                )
            except portal.PortalError as error:
                print(error)
                raise SystemExit(0)
            raise SystemExit("approval was bypassed")
            """
        )
        env["PACKAGE"] = str(PACKAGE)
        completed = subprocess.run(
            [sys.executable, "-c", script],
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )
        self.assertEqual(
            completed.returncode, 0, completed.stdout + completed.stderr
        )
        self.assertIn("no display", completed.stdout)

    def test_tool_error_redacts_secret_material(self):
        self.assertNotIn(SECRET, portal.redact(f"failed {SECRET}"))
        self.assertIn("[redacted]", portal.redact("ASIAIOSFODNN7EXAMPLE"))


if __name__ == "__main__":
    unittest.main()
