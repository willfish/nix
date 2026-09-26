#!/usr/bin/env python3
"""Announce new GitHub notification threads through the desktop bus."""

import json
import subprocess
import time
import urllib.parse
from pathlib import Path

POLL_SECONDS = 60
BURST_LIMIT = 8


def html_url(api_url):
    if not api_url:
        return ""
    parsed = urllib.parse.urlsplit(api_url)
    if parsed.netloc != "api.github.com":
        return ""
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 4 or parts[0] != "repos":
        return ""
    owner, repo, kind = parts[1], parts[2], parts[3]
    rest = parts[4:]
    if kind == "pulls" and rest:
        kind = "pull"
    elif kind == "commits" and rest:
        kind = "commit"
    elif kind not in {"issues", "pull", "commit", "discussions"}:
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
    return {
        "id": str(item.get("id") or ""),
        "repo": repo,
        "title": subject.get("title") or "GitHub notification",
        "reason": reason_label(item.get("reason")),
        "url": html_url(subject.get("url") or ""),
    }


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
        body = f"{row['reason']}: {row['title']}"
        if row["url"]:
            body = f"{body}\n{row['url']}"
        announcements.append({"summary": row["repo"], "body": body})
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


def announce(item):
    subprocess.run(
        [
            "notify-send",
            "-a",
            "GitHub",
            "-i",
            "github",
            "-u",
            "normal",
            "--",
            item["summary"],
            item["body"],
        ],
        check=False,
        timeout=10,
    )


def main():
    state_path = Path.home() / ".local/state/github-notifications/seen.json"
    while True:
        seen, seeded = load_state(state_path)
        try:
            items = fetch_notifications()
        except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
            items = None
        if items is not None:
            announcements, seen, seeded = plan(items, seen, seeded)
            for item in announcements:
                announce(item)
            save_state(state_path, seen, seeded)
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
