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
import urllib.request
from pathlib import Path

import websockets

CDP = os.environ.get("SLACK_CDP_URL", "http://127.0.0.1:9222")
OUT = Path(os.environ.get("OUT_FILE", Path.home() / ".config/slack-session/tokens.env"))


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

    tabs = json.loads(
        urllib.request.urlopen(f"{CDP}/json/list", timeout=5).read()
    )
    page = next(
        t
        for t in tabs
        if t.get("type") == "page"
        and t.get("webSocketDebuggerUrl")
        and "chrome-extension" not in (t.get("url") or "")
    )
    xoxc = None
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
        await call("Page.navigate", {"url": "https://app.slack.com/client"})
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
            if data.get("tokens"):
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
