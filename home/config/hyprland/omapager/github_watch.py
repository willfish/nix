#!/usr/bin/env python3
"""Announce new GitHub notification threads through the desktop bus.

A systemd user timer runs one pass and then this process exits. The Open
action is joined before exit so a click is not lost, but nothing stays
resident between polls.
"""

import json
import subprocess
import threading
import urllib.parse
from pathlib import Path

BURST_LIMIT = 8


def html_url(api_url):
    if not api_url:
        return ""
    parsed = urllib.parse.urlsplit(api_url)
    hosts = {"api.github.com", "github.com"}
    if parsed.scheme != "https" or parsed.netloc not in hosts:
        return ""
    if parsed.netloc == "github.com":
        if not parsed.path or parsed.path == "/":
            return ""
        return urllib.parse.urlunsplit(
            ("https", "github.com", parsed.path, "", "")
        )
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 4 or parts[0] != "repos":
        return ""
    owner, repo, kind = parts[1], parts[2], parts[3]
    rest = parts[4:]
    # /issues/comments/1 has no issue number, so it is not a page to open.
    if rest[:1] == ["comments"]:
        return ""
    if kind == "pulls" and rest:
        kind = "pull"
    elif kind == "commits" and rest:
        kind = "commit"
    elif kind == "issues" and len(rest) >= 3 and rest[1] == "comments":
        rest = rest[:1]
    elif kind not in {"issues", "pull", "commit", "discussions"}:
        return ""
    if not rest and kind in {"pull", "commit", "issues", "discussions"}:
        return ""
    return urllib.parse.urlunsplit(
        ("https", "github.com", "/".join([owner, repo, kind, *rest]), "", "")
    )


def reason_label(reason):
    labels = {
        "assign": "Assigned",
        "author": "Update",
        "comment": "Comment",
        "ci_activity": "CI",
        "invitation": "Invitation",
        "manual": "Subscribed",
        "mention": "Mention",
        "review_requested": "Review requested",
        "security_alert": "Security alert",
        "state_change": "State change",
        "subscribed": "Subscribed",
        "team_mention": "Team mention",
    }
    return labels.get(reason, "Notification")


def normalise(item):
    subject = item.get("subject") or {}
    repo = (item.get("repository") or {}).get("full_name") or "GitHub"
    url = html_url(subject.get("url") or "") or html_url(
        subject.get("latest_comment_url") or ""
    )
    return {
        "id": str(item.get("id") or ""),
        "repo": repo,
        "title": subject.get("title") or "GitHub notification",
        "reason": reason_label(item.get("reason")),
        "url": url,
    }


def message_body(row):
    body = f"{row['reason']}: {row['title']}"
    if row.get("url"):
        body = f"{body}\n{row['url']}"
    return body


def plan(items, seen, seeded):
    current = []
    for item in items:
        row = normalise(item)
        if row["id"]:
            current.append(row)
    if not seeded:
        summary = []
        if current:
            summary.append(
                {
                    "summary": "GitHub",
                    "body": (
                        f"{len(current)} unread notifications already waiting"
                    ),
                }
            )
        return summary, {row["id"] for row in current}, True
    fresh = [row for row in current if row["id"] not in seen]
    announcements = []
    shown = fresh[:BURST_LIMIT]
    for row in shown:
        announcements.append(
            {
                "summary": row["repo"],
                "body": message_body(row),
                "url": row["url"],
            }
        )
    if len(fresh) > BURST_LIMIT:
        announcements.append(
            {
                "summary": "GitHub",
                "body": f"{len(fresh) - BURST_LIMIT} more new notifications",
            }
        )
    return announcements, seen | {row["id"] for row in current}, True


def load_state(path):
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return set(), False
    return set(data.get("seen") or []), bool(data.get("seeded"))


def save_state(path, seen, seeded):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    payload = {"seeded": seeded, "seen": sorted(seen)}
    temporary.write_text(json.dumps(payload) + "\n")
    temporary.replace(path)


def fetch_notifications():
    completed = subprocess.run(
        ["gh", "api", "--paginate", "notifications"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        return None
    payload = json.loads(completed.stdout)
    if isinstance(payload, dict):
        return payload.get("items") or []
    return payload


def notify_command(item):
    command = [
        "notify-send",
        "-a",
        "GitHub",
        "-i",
        "github",
        "-u",
        "normal",
        "-t",
        "30000",
        "-h",
        "string:desktop-entry:github-notifications",
    ]
    if item.get("url"):
        # The action name is what the daemon prints back. Card click also
        # opens a bare https URL, but the button is the obvious control.
        command += ["-A", "open=Open"]
    command += ["--", item["summary"], item["body"]]
    return command


def chosen_action(output):
    return str(output or "").strip()


def open_chosen(output, url):
    if chosen_action(output) != "open" or not url:
        return False
    subprocess.run(["xdg-open", url], check=False, timeout=10)
    return True


def _finish_announce(proc, url):
    try:
        output, _ = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return
    open_chosen(output, url)


def announce(item):
    proc = subprocess.Popen(
        notify_command(item),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    thread = threading.Thread(
        target=_finish_announce,
        args=(proc, item.get("url") or ""),
    )
    thread.start()
    return thread


def main():
    state_path = Path.home() / ".local/state/github-notifications/seen.json"
    seen, seeded = load_state(state_path)
    try:
        items = fetch_notifications()
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return
    if items is None:
        return
    announcements, seen, seeded = plan(items, seen, seeded)
    threads = [announce(item) for item in announcements]
    save_state(state_path, seen, seeded)
    for thread in threads:
        thread.join()


if __name__ == "__main__":
    main()
