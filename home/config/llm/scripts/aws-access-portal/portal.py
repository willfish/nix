"""Brave CDP login and IAM Identity Center role credential export."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from constants import (
    ACCESS_KEYS,
    ACCOUNT_CELL,
    ACCOUNTS_TAB,
    ALERT,
    AUTH_COOKIE_NAMES,
    DEFAULT_REGION,
    DIRECTORY_ID,
    MFA_INPUT,
    MFA_SUBMIT,
    PASSWORD_INPUT,
    PASSWORD_SUBMIT,
    PORTAL_API,
    PORTAL_ORIGIN,
    PORTAL_START,
    PRODUCTION_ACCOUNT_ID,
    ROLE_LINK,
    SIGNIN_HOST,
    USERNAME_INPUT,
    USERNAME_SUBMIT,
)

ACCOUNT_ID_RE = re.compile(r"^\d{12}$")
ROLE_NAME_RE = re.compile(r"^[A-Za-z0-9+=,.@_-]{1,64}$")
ACCOUNT_NAME_RE = re.compile(r"^[A-Za-z0-9 ._+-]{1,64}$")
REGION_RE = re.compile(r"^[a-z]{2}-[a-z]+-\d$")
SECRET_RE = re.compile(
    r"(ASIA[A-Z0-9]{16}|AKIA[A-Z0-9]{16}|[A-Za-z0-9+/]{40,}={0,2})"
)


class PortalError(Exception):
    """Expected failure safe to show to the caller."""


def redact(text):
    return SECRET_RE.sub("[redacted]", str(text))


def needs_production_admin_approval(account_id, account_name, role_name):
    production = account_id == PRODUCTION_ACCOUNT_ID or (
        (account_name or "").strip().lower() == "production"
    )
    administrator = "administrator" in (role_name or "").lower()
    return production and administrator


def _shell_quote(value):
    if (
        not isinstance(value, str)
        or "'" in value
        or "\n" in value
        or "\r" in value
    ):
        raise PortalError("credential value has an unexpected character")
    return "'" + value + "'"


def state_dir():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        path = Path(runtime) / "aws-access-portal"
    else:
        path = Path.home() / ".local" / "state" / "aws-access-portal"
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chmod(path, 0o700)
    return path


def write_private(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chmod(path.parent, 0o700)
    fd, temporary = tempfile.mkstemp(prefix=".tmp-", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    os.chmod(path, 0o600)
    return path


def secrets_dir():
    configured = os.environ.get("SOPS_NIX_SECRETS_DIR")
    if configured:
        return Path(configured)
    config_home = os.environ.get(
        "XDG_CONFIG_HOME", str(Path.home() / ".config")
    )
    return Path(config_home) / "sops-nix" / "secrets"


def read_secret(name):
    path = secrets_dir() / name
    if not path.is_file() or not os.access(path, os.R_OK):
        raise PortalError(f"{name} is missing")
    value = path.read_text(encoding="utf-8").strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    if not value:
        raise PortalError(f"{name} is empty")
    return value


def fresh_totp(runner=None):
    second = int(time.time()) % 30
    if second >= 25:
        time.sleep(30 - second + 1)
    command = runner or ["totp-from-sops", "AWS"]
    try:
        completed = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise PortalError("could not generate an AWS TOTP code") from error
    code = completed.stdout.strip()
    if not re.fullmatch(r"\d{6}", code):
        raise PortalError("TOTP helper did not return a 6 digit code")
    return code


def request_approval(account_name, account_id, role_name):
    """Block for a local human decision. Tool arguments cannot approve this."""
    message = (
        "Export production administrator credentials?\n"
        f"Account: {account_name} ({account_id})\n"
        f"Role: {role_name}\n"
        "This grants production admin keys for a few hours.\n"
        "Select Approve only if you intend this. Agents must not answer."
    )
    if os.environ.get("AWS_PORTAL_ALLOW_APPROVAL_HOOK") == "1":
        command = os.environ.get("AWS_PORTAL_APPROVAL_COMMAND")
        if not command:
            raise PortalError("approval hook is enabled but no command is set")
        completed = subprocess.run(
            shlex.split(command),
            input=message,
            text=True,
            capture_output=True,
            timeout=90,
            check=False,
        )
        return (
            completed.returncode == 0 and completed.stdout.strip() == "approve"
        )

    if not os.environ.get("WAYLAND_DISPLAY") and not os.environ.get("DISPLAY"):
        raise PortalError(
            "production administrator credentials need a local "
            "approval prompt, and no display is available"
        )
    fuzzel = shutil.which("fuzzel")
    if not fuzzel:
        raise PortalError(
            "production administrator credentials need fuzzel for approval, "
            "and it is not on PATH"
        )
    completed = subprocess.run(
        [
            fuzzel,
            "--dmenu",
            "--prompt=Approve production admin? ",
            "--mesg",
            message,
            "--lines=2",
        ],
        input="Approve\nDeny\n",
        text=True,
        capture_output=True,
        timeout=90,
        check=False,
    )
    return completed.returncode == 0 and completed.stdout.strip() == "Approve"


@dataclass
class PortalClient:
    """HTTP and CDP operations. Tests replace the methods they need."""

    cdp_endpoint: str = "http://127.0.0.1:9222"
    api_base: str = PORTAL_API

    def browser_version(self):
        return self._get_json("/json/version")

    def _get_json(self, path):
        try:
            with urllib.request.urlopen(
                self.cdp_endpoint.rstrip("/") + path, timeout=5
            ) as response:
                return json.loads(response.read().decode())
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
            raise PortalError(
                f"Brave CDP is unavailable at {self.cdp_endpoint}"
            ) from error

    def cookies(self):
        version = self.browser_version()
        url = version.get("webSocketDebuggerUrl")
        if not url:
            raise PortalError(
                "Brave CDP did not provide a browser debugger URL"
            )
        socket = CdpSocket(url)
        try:
            return socket.call("Storage.getCookies").get("cookies") or []
        finally:
            socket.close()

    def auth_token(self):
        for cookie in self.cookies():
            domain = (cookie.get("domain") or "").lstrip(".")
            if cookie.get("name") not in AUTH_COOKIE_NAMES:
                continue
            if domain.endswith("awsapps.com") or domain.endswith(
                "amazonaws.com"
            ):
                value = cookie.get("value") or ""
                if value:
                    return value
        return None

    def api(self, token, path, query=None):
        params = ""
        if query:
            clean = {
                key: value for key, value in query.items() if value is not None
            }
            if clean:
                params = "?" + urllib.parse.urlencode(clean)
        request = urllib.request.Request(
            self.api_base + path + params,
            headers={
                "Authorization": f"Bearer {token}",
                "x-amz-sso-bearer-token": token,
                "x-amz-sso_bearer_token": token,
                "Accept": "application/json",
            },
            method="GET",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            raise PortalError(
                f"portal {path} returned HTTP {error.code}"
            ) from error
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
            raise PortalError(f"portal {path} request failed") from error

    def open_background_tab(self):
        version = self.browser_version()
        url = version.get("webSocketDebuggerUrl")
        if not url:
            raise PortalError(
                "Brave CDP did not provide a browser debugger URL"
            )
        return BackgroundTab(url)

    def page_state(self, page):
        expression = """(() => {
          const alert = document.querySelector(__ALERT__);
          return {
            href: location.href,
            username: !!document.querySelector(__USERNAME__),
            password: !!document.querySelector(__PASSWORD__),
            mfa: !!document.querySelector(__MFA__),
            accounts: !!document.querySelector(__ACCOUNTS__)
              || !!document.querySelector(__CELL__),
            alert: alert ? alert.innerText.slice(0, 180) : '',
            focused: document.hasFocus()
          };
        })()"""
        expression = (
            expression.replace("__ALERT__", json.dumps(ALERT))
            .replace("__USERNAME__", json.dumps(USERNAME_INPUT))
            .replace("__PASSWORD__", json.dumps(PASSWORD_INPUT))
            .replace("__MFA__", json.dumps(MFA_INPUT))
            .replace("__ACCOUNTS__", json.dumps(ACCOUNTS_TAB))
            .replace("__CELL__", json.dumps(ACCOUNT_CELL))
        )
        value = page.evaluate(expression)
        if not isinstance(value, dict):
            raise PortalError("could not read the portal page")
        return value

    def fill_and_submit(self, page, field, button, value):
        expression = """((field, button, value) => {
          const el = document.querySelector(field);
          const submit = document.querySelector(button);
          if (!el || !submit) return false;
          const proto = HTMLInputElement.prototype;
          const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
          setter.call(el, value);
          const opts = {bubbles: true, inputType: 'insertText', data: value};
          el.dispatchEvent(new InputEvent('input', opts));
          el.dispatchEvent(new Event('change', {bubbles: true}));
          submit.click();
          return true;
        })(%s, %s, %s)""" % (
            json.dumps(field),
            json.dumps(button),
            json.dumps(value),
        )
        return page.evaluate(expression) is True


class CdpSocket:
    def __init__(self, url):
        from websockets.sync.client import connect

        self.ws = connect(url, max_size=8_000_000)
        self.call_id = 0

    def call(self, method, params=None):
        if method in {"Target.activateTarget", "Page.bringToFront"}:
            raise PortalError(f"refusing to focus the browser with {method}")
        self.call_id += 1
        message = {"id": self.call_id, "method": method}
        if params:
            message["params"] = params
        self.ws.send(json.dumps(message))
        while True:
            raw = json.loads(self.ws.recv())
            if raw.get("id") != self.call_id:
                continue
            if "error" in raw:
                raise PortalError(f"CDP {method} failed")
            return raw.get("result") or {}

    def close(self):
        self.ws.close()


class BackgroundTab:
    """A CDP page created in the background and always closed."""

    def __init__(self, browser_ws):
        self.browser_ws = browser_ws
        self.target_id = None
        self.browser = None
        self.page = None

    def __enter__(self):
        self.browser = CdpSocket(self.browser_ws)
        created = self.browser.call(
            "Target.createTarget",
            {"url": "about:blank", "background": True},
        )
        self.target_id = created.get("targetId")
        if not self.target_id:
            self.browser.close()
            raise PortalError("could not create a background portal tab")
        page_url = self.browser_ws.rsplit("/devtools/browser/", 1)[0]
        page_url = page_url + "/devtools/page/" + self.target_id
        try:
            self.page = CdpSocket(page_url)
            self.page.call("Runtime.enable")
        except Exception:
            self.close()
            raise
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def close(self):
        target_id = self.target_id
        # Detach the debugger first. An attached session can keep the tab alive.
        if self.page is not None:
            self.page.close()
            self.page = None
        try:
            if self.browser is not None and target_id:
                self.browser.call("Target.closeTarget", {"targetId": target_id})
        finally:
            if self.browser is not None:
                self.browser.close()
                self.browser = None
        if target_id:
            self._wait_until_closed(target_id)

    def _wait_until_closed(self, target_id):
        parsed = urllib.parse.urlparse(self.browser_ws)
        list_url = f"http://{parsed.hostname}:{parsed.port}/json/list"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(list_url, timeout=2) as response:
                    tabs = json.loads(response.read().decode())
            except (OSError, urllib.error.URLError, json.JSONDecodeError):
                time.sleep(0.2)
                continue
            if not any(tab.get("id") == target_id for tab in tabs):
                return
            time.sleep(0.2)
        raise PortalError("background portal tab was not closed")

    def navigate(self, url):
        self.page.call("Page.navigate", {"url": url})

    def evaluate(self, expression):
        result = self.page.call(
            "Runtime.evaluate",
            {"expression": expression, "returnByValue": True},
        )
        remote = result.get("result") or {}
        if remote.get("subtype") == "error":
            raise PortalError("portal page script failed")
        return remote.get("value")


def _classify(state):
    href = state.get("href") or ""
    if (
        state.get("accounts")
        and PORTAL_ORIGIN in href
        and SIGNIN_HOST not in href
    ):
        return "portal"
    if state.get("mfa"):
        return "mfa"
    if state.get("password"):
        return "password"
    if state.get("username"):
        return "username"
    if SIGNIN_HOST in href or "signin.aws" in href:
        return "signin"
    return "unknown"


def login_if_needed(
    client, sleeper=time.sleep, totp=fresh_totp, secrets=read_secret
):
    token = client.auth_token()
    if token and _session_valid(client, token):
        return {"logged_in": True, "opened_tab": False}
    with client.open_background_tab() as page:
        page.navigate(PORTAL_START)
        deadline = time.monotonic() + 45
        state = {}
        while time.monotonic() < deadline:
            sleeper(0.4)
            state = client.page_state(page)
            kind = _classify(state)
            if kind == "portal":
                break
            if kind == "username":
                username = secrets("AWS_USERNAME")
                if not client.fill_and_submit(
                    page, USERNAME_INPUT, USERNAME_SUBMIT, username
                ):
                    raise PortalError("username field was not submitted")
                _wait_until(
                    client,
                    page,
                    sleeper,
                    lambda item: _classify(item) != "username",
                )
                continue
            if kind == "password":
                password = secrets("AWS_PASSWORD")
                if not client.fill_and_submit(
                    page, PASSWORD_INPUT, PASSWORD_SUBMIT, password
                ):
                    raise PortalError("password field was not submitted")
                _wait_until(
                    client,
                    page,
                    sleeper,
                    lambda item: _classify(item) != "password",
                )
                continue
            if kind == "mfa":
                _submit_mfa(client, page, sleeper, totp)
                break
        else:
            raise PortalError("timed out waiting for the access portal")
        final = client.page_state(page)
        if _classify(final) != "portal":
            alert = redact(
                final.get("alert") or "still not on the accounts tab"
            )
            raise PortalError(f"login did not reach the accounts tab: {alert}")
    token = client.auth_token()
    if not token or not _session_valid(client, token):
        raise PortalError("login finished but the portal session is not usable")
    return {"logged_in": True, "opened_tab": True}


def _submit_mfa(client, page, sleeper, totp):
    last_alert = ""
    for _attempt in range(2):
        code = totp()
        if not client.fill_and_submit(page, MFA_INPUT, MFA_SUBMIT, code):
            raise PortalError("MFA field was not submitted")
        end = time.monotonic() + 20
        while time.monotonic() < end:
            sleeper(0.4)
            state = client.page_state(page)
            if _classify(state) == "portal":
                return
            alert = state.get("alert") or ""
            if alert and alert != last_alert and _classify(state) == "mfa":
                last_alert = alert
                break
        else:
            break
    raise PortalError("MFA was rejected")


def _wait_until(client, page, sleeper, predicate):
    end = time.monotonic() + 25
    last = {}
    while time.monotonic() < end:
        sleeper(0.4)
        last = client.page_state(page)
        if last.get("alert") and predicate(last):
            raise PortalError(redact(last["alert"]))
        if predicate(last):
            return last
    raise PortalError("timed out waiting for the next sign-in step")


def _session_valid(client, token):
    try:
        body = client.api(token, "/token/whoAmI")
    except PortalError:
        return False
    expiry = body.get("expireDate")
    return (
        isinstance(expiry, (int, float))
        and expiry > time.time() * 1000 + 60_000
    )


def session_status(client):
    token = client.auth_token()
    if not token:
        return {"logged_in": False}
    try:
        body = client.api(token, "/token/whoAmI")
    except PortalError:
        return {"logged_in": False}
    expiry = body.get("expireDate")
    if not isinstance(expiry, (int, float)) or expiry <= time.time() * 1000:
        return {"logged_in": False}
    return {
        "logged_in": True,
        "directory_id": body.get("directoryId") or DIRECTORY_ID,
        "expires_at": _iso(expiry),
        "expires_in_seconds": max(0, int(expiry / 1000 - time.time())),
    }


def _iso(epoch_ms):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch_ms / 1000))


def _collect(client, token, path, list_key, query):
    items = []
    next_token = None
    for _page in range(20):
        payload = dict(query)
        if next_token:
            payload["next_token"] = next_token
        body = client.api(token, path, payload)
        items.extend(body.get(list_key) or [])
        next_token = body.get("nextToken")
        if not next_token:
            return items
    raise PortalError(f"{path} did not finish paging")


def list_accounts(client):
    token = _require_token(client)
    accounts = _collect(
        client, token, "/assignment/accounts", "accountList", {}
    )
    return [
        {
            "account_id": item.get("accountId"),
            "account_name": item.get("accountName"),
            "email": item.get("emailAddress"),
        }
        for item in accounts
    ]


def list_roles(client, account_id=None, account_name=None):
    account = resolve_account(client, account_id, account_name)
    token = _require_token(client)
    roles = _collect(
        client,
        token,
        "/assignment/roles",
        "roleList",
        {"account_id": account["account_id"]},
    )
    return {
        "account_id": account["account_id"],
        "account_name": account["account_name"],
        "roles": [item.get("roleName") for item in roles],
        "production_admin_approval_required": [
            item.get("roleName")
            for item in roles
            if needs_production_admin_approval(
                account["account_id"],
                account["account_name"],
                item.get("roleName"),
            )
        ],
    }


def resolve_account(client, account_id, account_name):
    if account_id is not None and not ACCOUNT_ID_RE.fullmatch(str(account_id)):
        raise PortalError("account_id must be a 12 digit AWS account id")
    if account_name is not None and not ACCOUNT_NAME_RE.fullmatch(account_name):
        raise PortalError("account_name has unexpected characters")
    if not account_id and not account_name:
        raise PortalError("account_id or account_name is required")
    accounts = list_accounts(client)
    chosen = accounts
    if account_id:
        chosen = [item for item in chosen if item["account_id"] == account_id]
    if account_name:
        wanted = account_name.casefold()
        chosen = [
            item
            for item in chosen
            if (item.get("account_name") or "").casefold() == wanted
        ]
    if not chosen:
        raise PortalError("no matching AWS account")
    if len(chosen) > 1:
        raise PortalError("account name is ambiguous")
    return chosen[0]


def _require_token(client):
    token = client.auth_token()
    if not token or not _session_valid(client, token):
        login_if_needed(client)
        token = client.auth_token()
    if not token:
        raise PortalError("portal session is missing after login")
    return token


def _env_path(account_id, role_name):
    return state_dir() / f"{account_id}-{role_name}.env"


def _read_cache(path):
    if not path.is_file():
        return None
    meta = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    for line in lines:
        if not line.startswith("# "):
            break
        key, separator, value = line[2:].partition("=")
        if separator:
            meta[key] = value
    expiry = meta.get("expiration")
    if not expiry or not expiry.isdigit():
        return None
    if int(expiry) <= time.time() * 1000 + 300_000:
        path.unlink(missing_ok=True)
        return None
    return meta


def export_credentials(
    client,
    role_name,
    account_id=None,
    account_name=None,
    region=DEFAULT_REGION,
    approver=request_approval,
):
    if not ROLE_NAME_RE.fullmatch(role_name or ""):
        raise PortalError("role_name is required and must be an IAM role name")
    if not REGION_RE.fullmatch(region or ""):
        raise PortalError("region must look like eu-west-2")
    account = resolve_account(client, account_id, account_name)
    if needs_production_admin_approval(
        account["account_id"], account["account_name"], role_name
    ):
        approved = approver(
            account["account_name"], account["account_id"], role_name
        )
        if not approved:
            raise PortalError(
                "production administrator credentials were not approved"
            )
    path = _env_path(account["account_id"], role_name)
    cached = _read_cache(path)
    if (
        cached
        and cached.get("account_id") == account["account_id"]
        and cached.get("role_name") == role_name
        and cached.get("region") == region
    ):
        return _public_export(
            account, role_name, region, int(cached["expiration"]), path, True
        )

    token = _require_token(client)
    body = client.api(
        token,
        "/federation/credentials",
        {"account_id": account["account_id"], "role_name": role_name},
    )
    creds = body.get("roleCredentials") or {}
    access_key = creds.get("accessKeyId")
    secret = creds.get("secretAccessKey")
    session = creds.get("sessionToken")
    expiry = creds.get("expiration")
    if not (
        isinstance(access_key, str)
        and access_key.startswith("ASIA")
        and isinstance(secret, str)
        and isinstance(session, str)
        and isinstance(expiry, (int, float))
    ):
        raise PortalError("portal did not return usable role credentials")
    text = "\n".join(
        [
            "# aws-access-portal v1",
            f"# account_id={account['account_id']}",
            f"# account_name={account['account_name']}",
            f"# role_name={role_name}",
            f"# expiration={int(expiry)}",
            f"# region={region}",
            "unset AWS_PROFILE",
            f"export AWS_ACCESS_KEY_ID={_shell_quote(access_key)}",
            f"export AWS_SECRET_ACCESS_KEY={_shell_quote(secret)}",
            f"export AWS_SESSION_TOKEN={_shell_quote(session)}",
            f"export AWS_DEFAULT_REGION={_shell_quote(region)}",
            f"export AWS_REGION={_shell_quote(region)}",
            "",
        ]
    )
    write_private(path, text)
    return _public_export(account, role_name, region, int(expiry), path, False)


def _export_note(account, role_name):
    note = (
        "Source env_file in the shell that needs AWS. "
        "Do not read or print it."
    )
    if needs_production_admin_approval(
        account["account_id"], account["account_name"], role_name
    ):
        note += (
            " Production administrator export already required "
            "local approval."
        )
    return note


def _public_export(account, role_name, region, expiry, path, cached):
    return {
        "account_id": account["account_id"],
        "account_name": account["account_name"],
        "role_name": role_name,
        "region": region,
        "expiration": _iso(expiry),
        "expires_in_seconds": max(0, int(expiry / 1000 - time.time())),
        "env_file": str(path),
        "cached": cached,
        "source": f"source {shlex.quote(str(path))}",
        "note": _export_note(account, role_name),
    }


# Referenced so a future DOM fallback can find the same controls we observed.
DOM_FALLBACK = {
    "account_cell": ACCOUNT_CELL,
    "role_link": ROLE_LINK,
    "access_keys": ACCESS_KEYS,
}
