"""Resolve Pi provider credentials without printing them.

Pi owns the login files. `pi auth print-bearer-token` starts the TUI in this
install, so collectors read the same files and refresh with the same token
requests Pi uses. OpenCode Go may be stored as a `!command` key; Pi runs that
command, and so does the collector.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

import usage_lib

CODEX_TOKEN_URL = "https://auth.openai.com/oauth/token"
CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
XAI_TOKEN_URL = "https://auth.x.ai/oauth2/token"
XAI_CLIENT_ID = "b1a00492-073a-47ea-816f-4c329264a828"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def agent_dir() -> Path:
    raw = os.environ.get("PI_AGENT_DIR") or str(Path.home() / ".pi/agent")
    return Path(os.path.expanduser(raw))


def read_json(path: Path) -> dict:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return payload if isinstance(payload, dict) else {}


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".auth-", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(payload, stream)
            stream.write("\n")
            stream.flush()
            os.fchmod(stream.fileno(), 0o600)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def refresh_oauth(url: str, body: dict[str, str]) -> dict:
    request = urllib.request.Request(
        url,
        data=urllib.parse.urlencode(body).encode("utf-8"),
        headers={
            "Accept": "application/json",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    opener = urllib.request.build_opener(_NoRedirect)
    with opener.open(request, timeout=15) as response:
        payload = json.loads(response.read().decode("utf-8", errors="replace"))
    if not isinstance(payload, dict) or not payload.get("access_token"):
        raise RuntimeError("refresh returned no access token")
    return payload


def oauth_access(provider: str, url: str, client_id: str) -> tuple[str, str]:
    """Return an access token and a short failure reason.

    The token is not logged.
    """

    path = agent_dir() / "auth.json"
    auth = read_json(path)
    entry = auth.get(provider)
    if not isinstance(entry, dict) or not (
        entry.get("refresh") or entry.get("access")
    ):
        return "", "Waiting for auth"
    now_ms = int(time.time() * 1000)
    if usage_lib.pi_token_current(entry, now_ms):
        return str(entry.get("access") or ""), ""
    refresh = str(entry.get("refresh") or "")
    if not refresh:
        return "", "Sign-in expired"
    try:
        payload = refresh_oauth(
            url,
            {
                "grant_type": "refresh_token",
                "refresh_token": refresh,
                "client_id": client_id,
            },
        )
    except Exception:
        if usage_lib.pi_token_current(entry, now_ms, skew_ms=0):
            return str(entry.get("access") or ""), ""
        return "", "Sign-in expired"
    auth[provider] = usage_lib.merge_pi_oauth(entry, payload, now_ms)
    try:
        write_json(path, auth)
    except OSError:
        pass
    return str(auth[provider].get("access") or ""), ""


def command_key(raw: str) -> str:
    value = raw.strip()
    if not value.startswith("!"):
        return value
    proc = subprocess.run(
        value[1:],
        shell=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    if proc.returncode != 0:
        return ""
    return proc.stdout.strip()


def opencode_go_key() -> str:
    env = os.environ.get("OPENCODE_GO_API_KEY") or ""
    if env:
        return env
    models = read_json(agent_dir() / "models.json")
    providers = (
        models.get("providers")
        if isinstance(models.get("providers"), dict)
        else {}
    )
    entry = (
        providers.get("opencode-go")
        if isinstance(providers.get("opencode-go"), dict)
        else {}
    )
    raw = entry.get("apiKey") or entry.get("key") or ""
    if not isinstance(raw, str) or not raw:
        auth = read_json(agent_dir() / "auth.json")
        stored = auth.get("opencode-go")
        if isinstance(stored, dict):
            raw = str(stored.get("key") or stored.get("access") or "")
        elif isinstance(stored, str):
            raw = stored
    if not raw:
        return ""
    return command_key(raw)
