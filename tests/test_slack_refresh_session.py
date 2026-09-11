import ast
import asyncio
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, patch


SCRIPT = (
    Path(__file__).parents[1]
    / "home/config/llm/scripts/slack-refresh-session.sh"
)
TOKEN = "xoxc-synthetic-test-token"
COOKIE = "xoxd-synthetic-test-cookie/+%2F=="


def tab(target_id, url):
    return {
        "id": target_id,
        "url": url,
        "type": "page",
        "webSocketDebuggerUrl": f"ws://fixture/{target_id}",
    }


class FakeBrowser:
    def __init__(self, tabs, *, evaluate_error=False, publish_target=True):
        self.tabs = [dict(item) for item in tabs]
        self.evaluate_error = evaluate_error
        self.publish_target = publish_target
        self.created = []
        self.closed = []
        self.navigations = []
        self.evaluated = []
        self.cookies = [{"name": "d", "domain": ".slack.com", "value": COOKIE}]
        self.auth_ok = True
        self.auth_error = False

    def urlopen(self, request, **kwargs):
        url = request if isinstance(request, str) else request.full_url
        if url.endswith("/json/version"):
            value = {"webSocketDebuggerUrl": "ws://fixture/browser"}
        elif url.endswith("/json/list"):
            value = [
                item
                for item in self.tabs
                if self.publish_target or item["id"] != "refresh-owned"
            ]
        elif url == "https://slack.com/api/auth.test":
            if self.auth_error:
                raise OSError("fixture auth unavailable")
            value = {"ok": self.auth_ok, "user": "fixture", "team": "fixture"}
        else:
            raise AssertionError(f"Unexpected request: {url}")
        return io.BytesIO(json.dumps(value).encode())

    def connect(self, url, **kwargs):
        browser = self
        target_id = url.rsplit("/", 1)[-1]

        class Socket:
            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                return False

            async def send(self, raw):
                request = json.loads(raw)
                method = request["method"]
                params = request.get("params", {})
                result = {}
                if method == "Storage.getCookies":
                    result = {"cookies": browser.cookies}
                elif method == "Target.createTarget":
                    browser.created.append(params)
                    browser.tabs.append(tab("refresh-owned", params["url"]))
                    result = {"targetId": "refresh-owned"}
                elif method == "Target.closeTarget":
                    browser.closed.append(params["targetId"])
                    browser.tabs = [
                        item
                        for item in browser.tabs
                        if item["id"] != params["targetId"]
                    ]
                    result = {"success": True}
                elif method == "Page.navigate":
                    browser.navigations.append((target_id, params["url"]))
                    next(
                        item for item in browser.tabs if item["id"] == target_id
                    )["url"] = params["url"]
                elif method == "Runtime.evaluate":
                    browser.evaluated.append(target_id)
                    if browser.evaluate_error:
                        self.reply = {
                            "id": request["id"],
                            "error": {"message": "fixture evaluation failure"},
                        }
                        return
                    page = next(
                        item for item in browser.tabs if item["id"] == target_id
                    )
                    result = {
                        "result": {
                            "value": json.dumps(
                                {"url": page["url"], "tokens": [TOKEN]}
                            )
                        }
                    }
                elif method not in {"Page.enable", "Runtime.enable"}:
                    raise AssertionError(f"Unexpected CDP method: {method}")
                self.reply = {"id": request["id"], "result": result}

            async def recv(self):
                return json.dumps(self.reply)

        return Socket()


class SlackRefreshSessionTest(unittest.TestCase):
    def run_refresh(self, browser, *, workspace=None, replace_error=False):
        source = (
            SCRIPT.read_text()
            .split("\"$PYTHON_BIN\" - <<'PY'\n", 1)[1]
            .split("\nPY\n", 1)[0]
        )
        tree = ast.parse(source)
        self.assertEqual(ast.unparse(tree.body.pop()), "asyncio.run(main())")
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            env = {
                "OUT_FILE": str(Path(directory) / "tokens.env"),
                "SLACK_CDP_URL": "http://fixture-cdp",
            }
            persisted = Path(env["OUT_FILE"])
            persisted.write_text("previous session\n")
            original_inode = persisted.stat().st_ino
            if workspace:
                env["SLACK_TEAM_ID"] = workspace
            with (
                patch.dict(os.environ, env, clear=True),
                patch.dict(
                    sys.modules,
                    {
                        "websockets": types.SimpleNamespace(
                            connect=browser.connect
                        )
                    },
                ),
                patch("urllib.request.urlopen", side_effect=browser.urlopen),
                patch("asyncio.sleep", new=AsyncMock()),
                patch("os.replace", side_effect=OSError(
                    "fixture replace failure"))
                if replace_error else contextlib.nullcontext(),
                contextlib.redirect_stdout(output),
                contextlib.redirect_stderr(output),
            ):
                namespace = {}
                exec(compile(tree, str(SCRIPT), "exec"), namespace)
                try:
                    asyncio.run(namespace["main"]())
                    failure = None
                except (Exception, SystemExit) as exc:
                    failure = exc
                if failure:
                    self.assertEqual(persisted.read_text(),
                                     "previous session\n")
                    self.assertEqual(persisted.stat().st_ino, original_inode)
                else:
                    self.assertEqual(persisted.read_text(
                    ), f"SLACK_COOKIE_D={COOKIE}\nSLACK_XOXC={TOKEN}\n")
                    self.assertNotEqual(persisted.stat().st_ino, original_inode)
                    self.assertEqual(persisted.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(
                        persisted.parent.stat().st_mode & 0o777, 0o700)
                self.assertEqual(list(persisted.parent.glob(".tokens-*")), [])
        self.assertNotIn(TOKEN, output.getvalue())
        self.assertNotIn(COOKIE, output.getvalue())
        return failure

    def assert_unrelated_preserved(self, browser):
        unrelated = next(
            item for item in browser.tabs if item["id"] == "unrelated"
        )
        self.assertEqual(unrelated["url"], "https://example.invalid/editor")
        self.assertEqual(browser.navigations, [])

    def test_rejects_lookalike_cookie_domains(self):
        for domain in ("evilslack.com", "slack.com.evil.invalid",
                       "notslack.invalid", "..slack.com", "evil@.slack.com"):
            with self.subTest(domain=domain):
                browser = FakeBrowser([])
                browser.cookies[0]["domain"] = domain
                self.assertIsNotNone(self.run_refresh(browser))
                self.assertEqual(browser.created, [])

    def test_accepts_legitimate_cookie_domains(self):
        for domain in ("slack.com", ".slack.com", "app.slack.com"):
            with self.subTest(domain=domain):
                browser = FakeBrowser([])
                browser.cookies[0]["domain"] = domain
                self.assertIsNone(self.run_refresh(browser))

    def test_auth_rejection_and_network_failure_preserve_previous_session(self):
        for network_error in (False, True):
            browser = FakeBrowser([])
            browser.auth_ok = False
            browser.auth_error = network_error
            self.assertIsNotNone(self.run_refresh(browser))

    def test_invalid_cookie_never_persists(self):
        browser = FakeBrowser([])
        browser.cookies[0]["value"] = "xoxd-test\nSLACK_XOXC=bad"
        self.assertIsNotNone(self.run_refresh(browser))

    def test_failed_atomic_replace_cleans_temporary_file(self):
        self.assertIsNotNone(self.run_refresh(
            FakeBrowser([]), replace_error=True))

    def test_reuses_matching_slack_tab_without_touching_other_pages(self):
        browser = FakeBrowser(
            [
                tab("unrelated", "https://example.invalid/editor"),
                tab(
                    "existing", "https://app.slack.com/client/TEXISTING/channel"
                ),
            ]
        )
        self.assertIsNone(self.run_refresh(browser))
        self.assert_unrelated_preserved(browser)
        self.assertEqual(browser.evaluated, ["existing"])
        self.assertEqual(browser.created, [])
        self.assertEqual(browser.closed, [])

    def test_creates_and_closes_only_its_own_tab(self):
        browser = FakeBrowser(
            [tab("unrelated", "https://example.invalid/editor")]
        )
        self.assertIsNone(self.run_refresh(browser))
        self.assert_unrelated_preserved(browser)
        self.assertEqual(browser.closed, ["refresh-owned"])
        self.assertEqual(
            browser.created,
            [{"url": "https://app.slack.com/client", "background": True}],
        )

    def test_new_tab_is_closed_when_token_extraction_fails(self):
        browser = FakeBrowser(
            [tab("unrelated", "https://example.invalid/editor")],
            evaluate_error=True,
        )
        self.assertIsNotNone(self.run_refresh(browser))
        self.assert_unrelated_preserved(browser)
        self.assertEqual(browser.closed, ["refresh-owned"])

    def test_new_tab_is_closed_when_target_discovery_fails(self):
        browser = FakeBrowser(
            [tab("unrelated", "https://example.invalid/editor")],
            publish_target=False,
        )
        self.assertIsNotNone(self.run_refresh(browser))
        self.assert_unrelated_preserved(browser)
        self.assertEqual(browser.closed, ["refresh-owned"])

    def test_existing_slack_tab_is_never_closed_on_failure(self):
        browser = FakeBrowser(
            [tab("existing", "https://app.slack.com/client/TEXISTING")],
            evaluate_error=True,
        )
        self.assertIsNotNone(self.run_refresh(browser))
        self.assertEqual(browser.closed, [])
        self.assertEqual(browser.navigations, [])

    def test_configured_workspace_selects_its_tab(self):
        browser = FakeBrowser(
            [
                tab("other-workspace", "https://app.slack.com/client/TOTHER"),
                tab(
                    "expected-workspace",
                    "https://app.slack.com/client/TEXPECTED/channel",
                ),
            ]
        )
        self.assertIsNone(self.run_refresh(browser, workspace="TEXPECTED"))
        self.assertEqual(browser.evaluated, ["expected-workspace"])
        self.assertEqual(browser.navigations, [])


if __name__ == "__main__":
    unittest.main()
