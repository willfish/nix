# shellcheck shell=bash
# prompt-capture: run an LLM CLI behind a local mitmproxy capture server.
#
# Built into a single executable by home/user/prompt-capture.nix, which
# prepends the shell shebang and the MITMDUMP closure path. Modes:
#
#   prompt-capture <codex|pi|grok> -- <command> [args...]
#   prompt-capture stop <tool>
#   prompt-capture logs <tool>
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage:
  prompt-capture <tool> -- <command> [args...]   capture a run of <command>
  prompt-capture stop <tool>                     stop a capture server left behind
  prompt-capture logs <tool>                     tail the capture file

tools: codex | pi | grok
USAGE
  exit 2
}

[ "$#" -ge 1 ] || usage

mode="$1"
shift

case "$mode" in
codex | pi | grok)
  tool="$mode"
  ;;
stop | logs)
  [ "$#" -eq 1 ] || usage
  tool="$1"
  ;;
*)
  usage
  ;;
esac

case "$tool" in
codex) port=8301 ;;
pi) port=8302 ;;
grok) port=8303 ;;
*) usage ;;
esac

state="${XDG_STATE_HOME:-$HOME/.local/state}"
cdir="$state/prompt-capture"
confdir="$cdir/mitmproxy"
pidfile="$cdir/$tool.pid"
jsonl="$cdir/$tool.jsonl"
serverlog="$cdir/$tool-server.log"
ca="$confdir/mitmproxy-ca.pem"
cafile="$cdir/$tool-ca.pem"

mkdir -p "$confdir"

port_open() {
  (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null
}

kill_pidfile() {
  local pid i
  pid="$(cat "$pidfile" 2>/dev/null || true)"
  if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    for i in $(seq 1 50); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
  fi
  rm -f "$pidfile"
}

if [ "$mode" = "stop" ]; then
  kill_pidfile
  echo "prompt-capture: $tool capture server stopped"
  exit 0
fi

if [ "$mode" = "logs" ]; then
  if [ -s "$jsonl" ]; then
    tail -n 50 "$jsonl"
  else
    echo "prompt-capture: no captures recorded yet for $tool ($jsonl)" >&2
    exit 1
  fi
  exit 0
fi

# Run mode: expect "-- <command> [args...]"
[ "$#" -ge 2 ] || usage
[ "$1" = "--" ] || {
  echo "prompt-capture: expected '--' before the command" >&2
  exit 2
}
shift
cmd=("$@")
[ "${#cmd[@]}" -ge 1 ] || usage

# Reap a capture server left behind by a crashed or killed run.
kill_pidfile

workdir="$(mktemp -d "$cdir/work.XXXXXX")"
run_id="$(date +%Y%m%dT%H%M%S)-$$"
srv_pid=""

cleanup() {
  local rc=$? i
  trap - EXIT INT TERM HUP
  if [ -n "$srv_pid" ] && kill -0 "$srv_pid" 2>/dev/null; then
    kill -TERM "$srv_pid" 2>/dev/null || true
    for i in $(seq 1 50); do
      kill -0 "$srv_pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "$srv_pid" 2>/dev/null || true
  fi
  rm -f "$pidfile"
  rm -rf "$workdir"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

cat >"$workdir/addon.py" <<'PYEOF'
import json
import os
import time

REDACTED_HEADERS = {
    "authorization",
    "proxy-authorization",
    "x-api-key",
    "api-key",
    "x-openrouter-api-key",
    "cookie",
}
MAX_CHARS = 200000

_jsonl = os.environ.get("PROMPT_CAPTURE_JSONL")
_tool = os.environ.get("PROMPT_CAPTURE_TOOL", "unknown")
_run = os.environ.get("PROMPT_CAPTURE_RUN", "")
_fh = open(_jsonl, "a", encoding="utf-8") if _jsonl else None


def _clip(text):
    if text is None:
        return ""
    if len(text) > MAX_CHARS:
        return text[:MAX_CHARS] + "...<truncated %d chars>" % (len(text) - MAX_CHARS)
    return text


def _clean(headers):
    return {
        k: "<redacted>" if k.lower() in REDACTED_HEADERS else v
        for k, v in headers.items()
    }


def _record(kind, flow):
    if _fh is None:
        return
    req = flow.request
    rec = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "run": _run,
        "tool": _tool,
        "kind": kind,
        "method": req.method,
        "url": req.pretty_host + req.path,
        "request_headers": _clean(req.headers),
        "request_body": _clip(req.get_text(strict=False)),
    }
    resp = flow.response
    if resp is not None:
        rec["status"] = resp.status_code
        if os.environ.get("PROMPT_CAPTURE_RESPONSE_BODY") == "1":
            rec["response_body"] = _clip(resp.get_text(strict=False))
    _fh.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
    _fh.flush()


class PromptCapture:
    def request(self, flow):
        try:
            _record("request", flow)
        except Exception as exc:
            print("prompt-capture addon error: %r" % (exc,), flush=True)

    def response(self, flow):
        try:
            _record("response", flow)
        except Exception as exc:
            print("prompt-capture addon error: %r" % (exc,), flush=True)

    def websocket_message(self, flow):
        # Codex-style backends stream the real payload over WebSocket frames
        # after the HTTP upgrade, so record text frames explicitly.
        try:
            if _fh is None or flow.websocket is None:
                return
            msg = flow.websocket.messages[-1]
            if msg.is_text:
                payload = msg.text
            elif msg.type.name == "BINARY":
                payload = repr(msg.content)
            else:
                return
            rec = {
                "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "run": _run,
                "tool": _tool,
                "kind": "ws_request" if msg.from_client else "ws_response",
                "method": "WS",
                "url": flow.request.pretty_host + flow.request.path,
                "data": _clip(payload),
            }
            _fh.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
            _fh.flush()
        except Exception as exc:
            print("prompt-capture addon error: %r" % (exc,), flush=True)


addons = [PromptCapture()]
PYEOF

PROMPT_CAPTURE_JSONL="$jsonl" \
  PROMPT_CAPTURE_TOOL="$tool" \
  PROMPT_CAPTURE_RUN="$run_id" \
  "$MITMDUMP" \
  --listen-host 127.0.0.1 \
  --listen-port "$port" \
  --set confdir="$confdir" \
  -s "$workdir/addon.py" \
  >"$serverlog" 2>&1 &
srv_pid=$!
echo "$srv_pid" >"$pidfile"

i=0
while [ "$i" -lt 100 ]; do
  if [ -s "$ca" ] && port_open; then
    break
  fi
  if ! kill -0 "$srv_pid" 2>/dev/null; then
    break
  fi
  i=$((i + 1))
  sleep 0.1
done

if ! port_open || [ ! -s "$ca" ]; then
  echo "prompt-capture: capture server did not start (see $serverlog)" >&2
  tail -n 20 "$serverlog" >&2 || true
  exit 1
fi

# Combined trust store: system bundle plus the mitmproxy CA.
cat /etc/ssl/certs/ca-bundle.crt "$ca" >"$cafile"

export HTTP_PROXY="http://127.0.0.1:$port"
export HTTPS_PROXY="$HTTP_PROXY"
export http_proxy="$HTTP_PROXY"
export https_proxy="$HTTP_PROXY"
export NO_PROXY="localhost,127.0.0.1${NO_PROXY:+,$NO_PROXY}"
export no_proxy="$NO_PROXY"
export SSL_CERT_FILE="$cafile"
export REQUESTS_CA_BUNDLE="$cafile"
export NODE_EXTRA_CA_CERTS="$ca"

echo "prompt-capture[$tool]: run $run_id, server pid $srv_pid on 127.0.0.1:$port" >&2
echo "prompt-capture[$tool]: prompts are logged to $jsonl" >&2

"${cmd[@]}"
