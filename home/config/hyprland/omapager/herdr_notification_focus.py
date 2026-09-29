#!/usr/bin/env python3
"""Open the Herdr pane behind a Ghostty desktop notification."""

import argparse
import json
import os
import socket
from pathlib import Path

SEPARATOR = " · "
EVENTS = ("needs attention", "finished")
STATUS_FOR_EVENT = {
    "finished": {"done", "idle"},
    "needs attention": {"blocked"},
}
CACHE_LIMIT = 100


def parse_toast(summary, body, app=""):
    """Return a toast target, or None when this is not a Herdr Ghostty toast."""
    summary = str(summary or "").strip()
    body = str(body or "").strip()
    app = str(app or "").strip().lower()
    ghostty = summary == "Ghostty" or "ghostty" in app
    if not ghostty:
        return None
    return _split_toast(_ghostty_text(summary, body))


def _ghostty_text(summary, body):
    summary = str(summary or "").strip()
    body = str(body or "").strip()
    if summary == "Ghostty":
        return body
    if body and ":" not in summary:
        return f"{summary}: {body}"
    return body or summary


def _split_toast(text):
    text = str(text or "").strip()
    event = None
    agent = None
    context = None
    for name in EVENTS:
        marker = f" {name}:"
        at = text.find(marker)
        if at <= 0:
            continue
        agent = text[:at].strip()
        context = text[at + len(marker) :].strip()
        event = name
        break
    if not agent or not context or event is None:
        return None
    parts = [part.strip() for part in context.split(SEPARATOR)]
    if len(parts) < 2 or not parts[1].isdigit():
        return None
    return {
        "agent": agent,
        "event": event,
        "workspace_label": parts[0],
        "workspace_number": int(parts[1]),
        "tab_label": SEPARATOR.join(parts[2:]) if len(parts) > 2 else None,
    }


def agent_matches(record, name):
    wanted = str(name or "").strip().lower()
    if not wanted:
        return False
    agent = str(record.get("agent") or "").strip().lower()
    display = str(record.get("display_agent") or "").strip().lower()
    if wanted in {agent, display}:
        return True
    return bool(agent) and (
        wanted.startswith(agent + " ") or wanted.startswith(agent + SEPARATOR)
    )


def resolve_target(snapshot, toast):
    """Pick a pane, otherwise a tab or workspace, from a session snapshot."""
    if not snapshot or not toast:
        return None
    workspace = _workspace(snapshot.get("workspaces") or [], toast)
    if not workspace:
        return None
    workspace_id = workspace.get("workspace_id")
    tabs = [
        tab
        for tab in snapshot.get("tabs") or []
        if tab.get("workspace_id") == workspace_id
    ]
    tab_label = toast.get("tab_label")
    tab = _tab(tabs, tab_label)
    if tab_label and not tab:
        return {"workspace_id": workspace_id}
    agents = [
        agent
        for agent in snapshot.get("agents") or []
        if agent.get("workspace_id") == workspace_id
        and agent_matches(agent, toast["agent"])
    ]
    if tab:
        in_tab = [
            agent
            for agent in agents
            if agent.get("tab_id") == tab.get("tab_id")
        ]
        pane = _pane(in_tab, toast["event"])
        if pane:
            return {
                "pane_id": pane.get("pane_id"),
                "tab_id": tab.get("tab_id"),
                "workspace_id": workspace_id,
            }
        return {"tab_id": tab.get("tab_id"), "workspace_id": workspace_id}
    pane = _pane(agents, toast["event"])
    if pane and _unique_status(agents, toast["event"]):
        return {
            "pane_id": pane.get("pane_id"),
            "tab_id": pane.get("tab_id"),
            "workspace_id": workspace_id,
        }
    return {"workspace_id": workspace_id}


def _workspace(workspaces, toast):
    label = toast["workspace_label"]
    number = toast["workspace_number"]
    labelled = [ws for ws in workspaces if ws.get("label") == label]
    numbered = [ws for ws in labelled if int(ws.get("number") or 0) == number]
    if numbered:
        return numbered[0]
    if len(labelled) == 1:
        return labelled[0]
    return None


def _tab(tabs, label):
    if label:
        exact = [tab for tab in tabs if tab.get("label") == label]
        return exact[0] if len(exact) == 1 else None
    if len(tabs) == 1:
        return tabs[0]
    return None


def _pane(agents, event):
    if not agents:
        return None
    wanted = STATUS_FOR_EVENT.get(event) or set()
    matching = [
        agent for agent in agents if agent.get("agent_status") in wanted
    ]
    pool = matching or agents
    return max(pool, key=lambda agent: int(agent.get("state_change_seq") or 0))


def _unique_status(agents, event):
    wanted = STATUS_FOR_EVENT.get(event) or set()
    matching = [
        agent for agent in agents if agent.get("agent_status") in wanted
    ]
    return len(matching) == 1


def cache_path(path=None):
    if path:
        return Path(path)
    state_home = os.environ.get("XDG_STATE_HOME")
    root = state_home or str(Path.home() / ".local" / "state")
    return Path(root) / "herdr-notification-focus" / "targets.json"


def cache_key(summary, body, ts):
    return f"{ts}|{summary}|{body}"


def remember(summary, body, ts, target, path=None):
    if not target:
        return None
    store = cache_path(path)
    data = _read_cache(store)
    key = cache_key(summary, body, ts)
    entry = {"key": key, **target}
    entries = [
        item for item in data.get("entries", []) if item.get("key") != key
    ]
    entries.append(entry)
    data["entries"] = entries[-CACHE_LIMIT:]
    _write_cache(store, data)
    return entry


def recall(summary, body, ts, path=None):
    key = cache_key(summary, body, ts)
    for item in reversed(_read_cache(cache_path(path)).get("entries", [])):
        if item.get("key") == key:
            return item
    return None


def _read_cache(path):
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {"entries": []}
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        return {"entries": []}
    return data


def _write_cache(path, data):
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data))
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def socket_paths(env=None, home=None):
    env = env if env is not None else os.environ
    home = Path(home) if home else Path.home()
    config = Path(env.get("XDG_CONFIG_HOME") or home / ".config") / "herdr"
    paths = []
    override = env.get("HERDR_SOCKET_PATH")
    if override:
        paths.append(Path(override))
    paths.append(config / "herdr.sock")
    sessions = config / "sessions"
    if sessions.is_dir():
        paths.extend(sorted(sessions.glob("*/herdr.sock")))
    seen = set()
    existing = []
    for path in paths:
        key = str(path)
        if key in seen or not path.exists():
            continue
        seen.add(key)
        existing.append(path)
    return existing


def herdr_call(sock_path, method, params, opener=None):
    opener = opener or socket.socket
    payload = {"id": "omapager-herdr-focus", "method": method, "params": params}
    with opener(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(2)
        conn.connect(str(sock_path))
        conn.sendall(json.dumps(payload).encode() + b"\n")
        data = b""
        while b"\n" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
    if not data:
        return None
    return json.loads(data.split(b"\n", 1)[0])


def snapshot_from(response):
    result = (response or {}).get("result") or {}
    snapshot = result.get("snapshot")
    return snapshot if isinstance(snapshot, dict) else None


def focus_payload(target):
    if not target:
        return None
    if target.get("pane_id"):
        return "pane.focus", {"pane_id": target["pane_id"]}
    if target.get("tab_id"):
        return "tab.focus", {"tab_id": target["tab_id"]}
    if target.get("workspace_id"):
        return "workspace.focus", {"workspace_id": target["workspace_id"]}
    return None


def choose_window(clients, pid=0, ancestors=None, workspace_label=""):
    clients = list(clients or [])
    ancestors = set(ancestors or [])
    if pid:
        for client in clients:
            if int(client.get("pid") or 0) == int(pid):
                return client
    attached = [
        client
        for client in clients
        if int(client.get("pid") or 0) in ancestors
    ]
    pool = attached or _ghostty_clients(clients)
    ghostty = _ghostty_clients(pool) or pool
    if not attached and workspace_label:
        titled = [
            client
            for client in ghostty
            if workspace_label in str(client.get("title") or "")
        ]
        if len(titled) == 1:
            return titled[0]
    if not ghostty:
        return None
    return min(
        ghostty,
        key=lambda client: int(client.get("focusHistoryID") or 0),
    )


def _ghostty_clients(clients):
    return [
        client
        for client in clients
        if "ghostty" in str(client.get("class") or "").lower()
        and "cliamp" not in str(client.get("class") or "").lower()
    ]


def parent_pid(pid, reader=None):
    if not pid:
        return 0
    try:
        text = (reader or _read_stat)(pid)
    except OSError:
        return 0
    end = text.rfind(")")
    if end < 0:
        return 0
    fields = text[end + 2 :].split()
    if len(fields) < 2:
        return 0
    try:
        return int(fields[1])
    except ValueError:
        return 0


def _read_stat(pid):
    return Path(f"/proc/{pid}/stat").read_text()


def ancestor_pids(start, parent=parent_pid, limit=32):
    found = []
    pid = int(start or 0)
    seen = set()
    while pid and pid not in seen and len(found) < limit:
        seen.add(pid)
        found.append(pid)
        pid = int(parent(pid) or 0)
    return found


def run_remember(summary, body, ts, sockets, call, cache):
    toast = parse_toast(summary, body)
    if not toast:
        return None
    for sock in sockets:
        response = call(sock, "session.snapshot", {})
        target = resolve_target(snapshot_from(response), toast)
        if not target:
            continue
        target = {**target, "socket": str(sock)}
        return remember(summary, body, ts, target, cache)
    return None


def target_alive(snapshot, target):
    if not snapshot or not target:
        return False
    pane_id = target.get("pane_id")
    if pane_id:
        panes = (snapshot.get("agents") or []) + (snapshot.get("panes") or [])
        ids = {item.get("pane_id") for item in panes}
        return pane_id in ids
    tab_id = target.get("tab_id")
    if tab_id:
        return any(
            tab.get("tab_id") == tab_id for tab in snapshot.get("tabs") or []
        )
    workspace_id = target.get("workspace_id")
    return any(
        workspace.get("workspace_id") == workspace_id
        for workspace in snapshot.get("workspaces") or []
    )


def run_open(
    summary,
    body,
    ts,
    pid,
    sockets,
    call,
    cache,
    clients,
    parents,
    socket_pids=None,
):
    toast = parse_toast(summary, body)
    stored = recall(summary, body, ts, cache) if toast else None
    target = None
    sock = None
    if stored and stored.get("socket"):
        sock = Path(stored["socket"])
        snap = snapshot_from(call(sock, "session.snapshot", {}))
        if target_alive(snap, stored):
            target = stored
        elif toast:
            target = resolve_target(snap, toast)
    if toast and not target:
        for candidate in sockets:
            snap = snapshot_from(call(candidate, "session.snapshot", {}))
            resolved = resolve_target(snap, toast)
            if resolved:
                sock = candidate
                target = resolved
                break
    if sock and target:
        payload = focus_payload(target)
        if payload:
            call(sock, payload[0], payload[1])
    window = None
    if clients is not None:
        ancestors = []
        finder = socket_pids or _attached_pids
        if sock:
            for holder in finder(sock):
                ancestors.extend(ancestor_pids(holder, parents))
        window = choose_window(
            clients,
            pid=pid,
            ancestors=ancestors,
            workspace_label=(toast or {}).get("workspace_label", ""),
        )
    return {"target": target, "window": window}


def _attached_pids(path, proc=Path("/proc")):
    session = _session_name(path)
    matched = []
    fallback = []
    for cmd_path in proc.glob("[0-9]*/cmdline"):
        try:
            raw = cmd_path.read_bytes().replace(b"\0", b" ")
            text = raw.decode(errors="ignore")
            pid = int(cmd_path.parent.name)
        except (OSError, ValueError):
            continue
        if "herdr" not in text or "session attach" not in text:
            continue
        fallback.append(pid)
        if session is None and "session attach default" in text:
            matched.append(pid)
        elif session and f"session attach {session}" in text:
            matched.append(pid)
    return matched or fallback


def _session_name(path):
    parts = Path(path).parts
    if "sessions" not in parts:
        return None
    index = parts.index("sessions")
    if index + 1 >= len(parts) - 1:
        return None
    return parts[index + 1]


def main(argv=None):
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("remember", "open"):
        command = commands.add_parser(name)
        command.add_argument("--summary", default="")
        command.add_argument("--body", default="")
        command.add_argument("--ts", default="")
        command.add_argument("--pid", default="0")
    args = parser.parse_args(argv)
    sockets = socket_paths()
    if args.command == "remember":
        run_remember(
            args.summary, args.body, args.ts, sockets, herdr_call, None
        )
        return 0
    pid = int(args.pid) if str(args.pid).isdigit() else 0
    outcome = run_open(
        args.summary,
        args.body,
        args.ts,
        pid,
        sockets,
        herdr_call,
        None,
        _hypr_clients(),
        parent_pid,
    )
    window = outcome.get("window")
    if window and window.get("address"):
        _focus_address(window["address"])
    return 0


def _hypr_clients():
    import subprocess

    try:
        completed = subprocess.run(
            ["hyprctl", "clients", "-j"],
            check=False,
            capture_output=True,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if completed.returncode != 0 or not completed.stdout.strip():
        return []
    try:
        clients = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return []
    return clients if isinstance(clients, list) else []


def _focus_address(address):
    import subprocess

    subprocess.run(
        ["hyprctl", "dispatch", "focuswindow", f"address:{address}"],
        check=False,
        timeout=2,
    )


if __name__ == "__main__":
    raise SystemExit(main())
