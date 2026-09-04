#!/usr/bin/env bash
# Extract Slack browser session tokens (xoxc + cookie d) via Brave CDP.
set -euo pipefail

export SLACK_CDP_URL="${SLACK_CDP_URL:-http://127.0.0.1:9222}"
OUT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/slack-session"
export OUT_FILE="${OUT_FILE:-$OUT_DIR/tokens.env}"
PYTHON_BIN="${SLACK_REFRESH_PYTHON:-python3}"

mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"

"$PYTHON_BIN" - <<'PY'
import asyncio
import json
import os
import sys
import urllib.request
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import quote, urlsplit

import websockets

CDP = os.environ.get("SLACK_CDP_URL", "http://127.0.0.1:9222")
OUT = Path(os.environ.get("OUT_FILE", Path.home() / ".config/slack-session/tokens.env"))
WORKSPACE = (os.environ.get("SLACK_TEAM_ID") or "").strip()
SLACK_CLIENT = "https://app.slack.com/client"
if WORKSPACE:
    SLACK_CLIENT += "/" + quote(WORKSPACE, safe="")


async def cdp_call(ws, n, method, params=None):
    msg = {"id": n, "method": method}
    if params:
        msg["params"] = params
    await ws.send(json.dumps(msg))
    while True:
        raw = json.loads(await ws.recv())
        if raw.get("id") == n:
            if "error" in raw:
                raise RuntimeError(raw["error"])
            return raw.get("result", {})


def is_slack_client(url):
    parsed = urlsplit(url or "")
    path = parsed.path.split("/")
    return (
        parsed.scheme == "https"
        and parsed.netloc == "app.slack.com"
        and path[1:2] == ["client"]
        and (not WORKSPACE or path[2:3] == [quote(WORKSPACE, safe="")])
    )


def browser_tabs():
    return json.loads(
        urllib.request.urlopen(f"{CDP}/json/list", timeout=5).read()
    )


@asynccontextmanager
async def slack_client_page(browser_url):
    page = next((
        tab for tab in browser_tabs()
        if tab.get("type") == "page"
        and tab.get("webSocketDebuggerUrl")
        and is_slack_client(tab.get("url"))
    ), None)
    if page is not None:
        yield page
        return

    async with websockets.connect(browser_url, max_size=50_000_000) as ws:
        created = await cdp_call(ws, 1, "Target.createTarget", {
            "url": SLACK_CLIENT, "background": True,
        })
        target_id = created.get("targetId")
        if not target_id:
            raise RuntimeError("Could not create a temporary Slack refresh tab")
        try:
            for _ in range(20):
                page = next((
                    tab for tab in browser_tabs()
                    if tab.get("id") == target_id
                    and tab.get("webSocketDebuggerUrl")
                ), None)
                if page is not None:
                    yield page
                    return
                await asyncio.sleep(0.25)
            raise RuntimeError("Could not connect to the temporary Slack refresh tab")
        finally:
            try:
                await asyncio.wait_for(cdp_call(ws, 2, "Target.closeTarget", {
                    "targetId": target_id,
                }), timeout=5)
            except Exception:
                print("warning: could not close temporary Slack refresh tab", file=sys.stderr)


async def main():
    version = json.loads(
        urllib.request.urlopen(f"{CDP}/json/version", timeout=5).read()
    )
    # cookies from browser target
    cookie_d = None
    async with websockets.connect(
        version["webSocketDebuggerUrl"], max_size=50_000_000
    ) as ws:
        res = await cdp_call(ws, 1, "Storage.getCookies")
        for c in res.get("cookies") or []:
            if c.get("name") == "d" and "slack" in c.get("domain", ""):
                cookie_d = c.get("value")
                break
    if not cookie_d or not str(cookie_d).startswith("xoxd-"):
        raise SystemExit(
            "no Slack cookie d=xoxd-… found; log into Slack in Brave first"
        )

    xoxc = None
    async with slack_client_page(version["webSocketDebuggerUrl"]) as page:
        async with websockets.connect(
            page["webSocketDebuggerUrl"], max_size=50_000_000
        ) as ws:
            n = 0

            async def call(method, params=None):
                nonlocal n
                n += 1
                return await cdp_call(ws, n, method, params)

            await call("Page.enable")
            await call("Runtime.enable")
            for _ in range(45):
                await asyncio.sleep(1)
                res = await call(
                    "Runtime.evaluate",
                    {
                        "expression": """(() => {
                          const html = document.documentElement.innerHTML;
                          const m = html.match(/xoxc-[A-Za-z0-9-]+/g) || [];
                          const tokens = [];
                          try {
                            for (let i = 0; i < localStorage.length; i++) {
                              const v = localStorage.getItem(localStorage.key(i)) || '';
                              const mm = v.match(/xoxc-[A-Za-z0-9-]+/g) || [];
                              tokens.push(...mm);
                            }
                          } catch (e) {}
                          return JSON.stringify({
                            url: location.href,
                            tokens: [...new Set([...m, ...tokens])]
                          });
                        })()""",
                        "returnByValue": True,
                    },
                )
                val = res.get("result", {}).get("value")
                if not val:
                    continue
                data = json.loads(val)
                if is_slack_client(data.get("url")) and data.get("tokens"):
                    xoxc = data["tokens"][0]
                    break
    if not xoxc:
        raise SystemExit("failed to find xoxc token from Slack client page")

    OUT.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    OUT.write_text(
        f"SLACK_COOKIE_D={cookie_d}\nSLACK_XOXC={xoxc}\n",
        encoding="utf-8",
    )
    os.chmod(OUT, 0o600)
    # Do not print secrets
    print(f"wrote {OUT} (xoxc+cookie d)")
    # quick auth check
    import urllib.request as u

    req = u.Request(
        "https://slack.com/api/auth.test",
        headers={
            "Authorization": f"Bearer {xoxc}",
            "Cookie": f"d={cookie_d}",
        },
        method="POST",
    )
    with u.urlopen(req, timeout=30) as resp:
        payload = json.loads(resp.read().decode())
    if not payload.get("ok"):
        raise SystemExit(f"auth.test failed: {payload.get('error')}")
    print(
        f"auth ok user={payload.get('user')} team={payload.get('team')}"
    )


asyncio.run(main())
PY

# Optionally re-import into sops when running inside ~/.dotfiles checkout
if [ "${SLACK_UPDATE_SOPS:-1}" = "1" ] && command -v sops >/dev/null 2>&1; then
  repo="${SLACK_DOTFILES_ROOT:-$HOME/.dotfiles}"
  secrets_file="$repo/secrets/env.yaml"
  if [ -r "$secrets_file" ] && [ -r "$OUT_FILE" ]; then
    # shellcheck disable=SC1090
    set -a
    # shellcheck disable=SC1090
    . "$OUT_FILE"
    set +a
    if [ -n "${SLACK_XOXC:-}" ] && [ -n "${SLACK_COOKIE_D:-}" ]; then
      python3 - <<'PY'
import json, os, subprocess, pathlib
repo = pathlib.Path(os.environ.get("SLACK_DOTFILES_ROOT", pathlib.Path.home() / ".dotfiles"))
secrets = repo / "secrets" / "env.yaml"
for key in ("SLACK_XOXC", "SLACK_COOKIE_D"):
    val = os.environ[key]
    subprocess.check_call(
        ["sops", "set", str(secrets), f'["{key}"]', json.dumps(val)],
        cwd=repo,
    )
    print(f"sops updated {key}")
PY
      echo "note: run home-manager switch to re-render ~/.config/sops-nix/secrets"
    fi
  fi
fi
