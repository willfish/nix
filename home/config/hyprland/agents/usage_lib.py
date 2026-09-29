"""Display records for the pinned Omarchy agents panel.

Collectors print one JSON object. They never print credentials. The panel
only reads the files the updater writes.
"""

from __future__ import annotations

import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


def number(value: Any) -> int:
    try:
        parsed = float(value or 0)
    except (TypeError, ValueError):
        return 0
    if parsed != parsed:
        return 0
    return round(parsed)


def local_day(value: Any, fallback: datetime | None = None) -> str:
    now = fallback or datetime.now().astimezone()
    if value is None or value == "":
        return now.strftime("%Y-%m-%d")
    if isinstance(value, (int, float)):
        seconds = (
            float(value) / 1000.0
            if float(value) > 10_000_000_000
            else float(value)
        )
        try:
            return (
                datetime.fromtimestamp(seconds)
                .astimezone()
                .strftime("%Y-%m-%d")
            )
        except (OSError, OverflowError, ValueError):
            return now.strftime("%Y-%m-%d")
    raw = str(value).strip()
    if raw.isdigit():
        return local_day(int(raw), now)
    try:
        parsed = datetime.fromisoformat(raw.replace("Z", "+00:00"))
    except ValueError:
        return now.strftime("%Y-%m-%d")
    if parsed.tzinfo is not None:
        parsed = parsed.astimezone()
    return parsed.strftime("%Y-%m-%d")


def recent_dates(today: datetime | None = None) -> list[str]:
    day = (today or datetime.now().astimezone()).date()
    return [
        (day - timedelta(days=offset)).strftime("%Y-%m-%d")
        for offset in range(6, -1, -1)
    ]


def empty_bucket() -> dict[str, int]:
    return {
        "inputTokens": 0,
        "outputTokens": 0,
        "cacheReadInputTokens": 0,
        "cacheCreationInputTokens": 0,
    }


def empty_stats(today: datetime | None = None) -> dict[str, Any]:
    dates = recent_dates(today)
    return {
        "todayPrompts": 0,
        "todaySessions": 0,
        "todayTotalTokens": 0,
        "todayTokensByModel": {},
        "recentDays": [{"date": day, "messageCount": 0} for day in dates],
        "totalPrompts": 0,
        "totalSessions": 0,
        "activeDays": 0,
        "activeDates": [],
        "modelUsage": {},
    }


def _add_model(
    stats: dict[str, Any],
    *,
    day: str,
    today: str,
    session_key: str,
    model: str,
    input_tokens: int,
    output_tokens: int,
    cache_read: int,
    cache_write: int,
    prompts: int,
    sessions: set[str],
    today_sessions: set[str],
    active_days: set[str],
) -> None:
    total = input_tokens + output_tokens + cache_read + cache_write
    if total <= 0 and prompts <= 0:
        return
    sessions.add(session_key)
    active_days.add(day)
    stats["totalPrompts"] += prompts
    if day == today:
        today_sessions.add(session_key)
        stats["todayPrompts"] += prompts
        stats["todayTotalTokens"] += total
        if total:
            by_model = stats["todayTokensByModel"]
            by_model[model] = by_model.get(model, 0) + total
    if total:
        bucket = stats["modelUsage"].setdefault(model, empty_bucket())
        bucket["inputTokens"] += input_tokens
        bucket["outputTokens"] += output_tokens
        bucket["cacheReadInputTokens"] += cache_read
        bucket["cacheCreationInputTokens"] += cache_write
        for row in stats["recentDays"]:
            if row["date"] == day:
                row["messageCount"] += total


def finalize_stats(
    stats: dict[str, Any],
    sessions: set[str],
    today_sessions: set[str],
    active_days: set[str],
) -> dict[str, Any]:
    stats["totalSessions"] = len(sessions)
    stats["todaySessions"] = len(today_sessions)
    stats["activeDays"] = len(active_days)
    stats["activeDates"] = sorted(active_days)
    return stats


def split_grok_tokens(usage: dict[str, Any]) -> tuple[int, int, int, int]:
    cache_read = number(
        usage.get("cachedReadTokens", usage.get("cacheReadInputTokens"))
    )
    cache_write = number(
        usage.get("cacheCreationTokens", usage.get("cacheCreationInputTokens"))
    )
    raw_input = number(usage.get("inputTokens"))
    output_tokens = number(usage.get("outputTokens")) + number(
        usage.get("reasoningTokens")
    )
    input_tokens = (
        max(0, raw_input - cache_read) if cache_read <= raw_input else raw_input
    )
    return input_tokens, output_tokens, cache_read, cache_write


def _grok_snapshot(usage: dict[str, Any]) -> dict[str, Any] | None:
    if not isinstance(usage, dict):
        return None
    total = number(usage.get("totalTokens"))
    split = split_grok_tokens(usage)
    if total <= 0 and not any(split):
        return None
    return usage


def scan_grok_sessions(
    sessions_root: Path, today: datetime | None = None
) -> dict[str, Any]:
    """Count each Grok session once.

    turn_completed usage objects are session snapshots, not deltas: the same
    session reports a new total as it grows, and the totals are not monotonic
    when subagents finish. The largest snapshot is the session total.
    """

    now = today or datetime.now().astimezone()
    today_key = now.strftime("%Y-%m-%d")
    stats = empty_stats(now)
    sessions: set[str] = set()
    today_sessions: set[str] = set()
    active_days: set[str] = set()
    winners: dict[str, tuple[int, str, dict[str, Any]]] = {}
    if not sessions_root.is_dir():
        return finalize_stats(stats, sessions, today_sessions, active_days)

    for path in sessions_root.rglob("updates.jsonl"):
        if not path.is_file():
            continue
        try:
            lines = path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
        except OSError:
            continue
        for line in lines:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(event, dict):
                continue
            params = (
                event.get("params")
                if isinstance(event.get("params"), dict)
                else {}
            )
            update = (
                params.get("update")
                if isinstance(params.get("update"), dict)
                else {}
            )
            if update.get("sessionUpdate") != "turn_completed":
                continue
            usage = _grok_snapshot(update.get("usage"))
            if usage is None:
                continue
            session_id = str(params.get("sessionId") or path.parent.name)
            total = number(usage.get("totalTokens")) or sum(
                split_grok_tokens(usage)
            )
            day = local_day(event.get("timestamp"), now)
            current = winners.get(session_id)
            if current is None or total >= current[0]:
                winners[session_id] = (total, day, usage)

    for session_id, (_total, day, usage) in winners.items():
        prompts = max(1, number(usage.get("numTurns")))
        models = usage.get("modelUsage")
        emitted = False
        if isinstance(models, dict):
            for name, bucket in models.items():
                if not isinstance(bucket, dict):
                    continue
                split = split_grok_tokens(bucket)
                if not any(split):
                    continue
                _add_model(
                    stats,
                    day=day,
                    today=today_key,
                    session_key=session_id,
                    model=str(name or "grok"),
                    input_tokens=split[0],
                    output_tokens=split[1],
                    cache_read=split[2],
                    cache_write=split[3],
                    prompts=prompts if not emitted else 0,
                    sessions=sessions,
                    today_sessions=today_sessions,
                    active_days=active_days,
                )
                emitted = True
        if not emitted:
            split = split_grok_tokens(usage)
            _add_model(
                stats,
                day=day,
                today=today_key,
                session_key=session_id,
                model="grok",
                input_tokens=split[0],
                output_tokens=split[1],
                cache_read=split[2],
                cache_write=split[3],
                prompts=prompts,
                sessions=sessions,
                today_sessions=today_sessions,
                active_days=active_days,
            )
    return finalize_stats(stats, sessions, today_sessions, active_days)


def fraction(value: Any) -> float:
    try:
        parsed = float(str(value).strip().replace("%", ""))
    except (TypeError, ValueError):
        return -1.0
    if parsed < 0 or parsed != parsed:
        return -1.0
    if parsed > 1:
        parsed = parsed / 100.0
    return min(1.0, parsed)


def parse_grok_billing(payload: dict[str, Any]) -> list[dict[str, str | float]]:
    config = (
        payload.get("config")
        if isinstance(payload.get("config"), dict)
        else payload
    )
    if not isinstance(config, dict):
        return []
    period = config.get("currentPeriod") or config.get("current_period") or {}
    end = ""
    if isinstance(period, dict):
        end = str(period.get("end") or "")
    if not end:
        end = str(
            config.get("billingPeriodEnd")
            or config.get("billing_period_end")
            or ""
        )
    kind = (
        str(period.get("type") or "").upper()
        if isinstance(period, dict)
        else ""
    )
    label = "Monthly" if "MONTH" in kind else "Weekly (7-day)"
    limits: list[dict[str, str | float]] = []
    overall = fraction(
        config.get("creditUsagePercent", config.get("credit_usage_percent"))
    )
    if overall >= 0:
        limits.append(
            {
                "label": label,
                "title": "Weekly" if label.startswith("Weekly") else "Monthly",
                "percent": overall,
                "resetsAt": end,
            }
        )
    rows = config.get("productUsage") or config.get("product_usage") or []
    if isinstance(rows, list):
        for row in rows:
            if (
                not isinstance(row, dict)
                or str(row.get("product") or "") != "GrokBuild"
            ):
                continue
            build = fraction(row.get("usagePercent", row.get("usage_percent")))
            if build >= 0:
                limits.append(
                    {
                        "label": "Grok Build",
                        "title": "Grok Build",
                        "percent": build,
                        "resetsAt": end,
                    }
                )
    return limits


def grok_tier_label(claims: dict[str, Any]) -> str:
    named = str(
        claims.get("subscription_tier")
        or claims.get("subscriptionTier")
        or claims.get("plan")
        or ""
    ).strip()
    if named:
        return named.replace("_", " ")
    labels = {
        0: "Free",
        1: "SuperGrok",
        2: "X Basic",
        3: "X Premium",
        4: "X Premium+",
        5: "SuperGrok Heavy",
        6: "SuperGrok Lite",
        7: "SuperGrok+",
    }
    try:
        return labels.get(int(claims.get("tier")), "")
    except (TypeError, ValueError):
        return ""


def choose_grok_login(
    auth: dict[str, Any],
) -> tuple[str, dict[str, Any]] | None:
    chosen: tuple[str, dict[str, Any]] | None = None
    for key, value in auth.items():
        if not isinstance(value, dict) or not value.get("key"):
            continue
        chosen = (str(key), value)
        expires = str(value.get("expires_at") or "")
        if expires == "" or expires > datetime.now(timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%S"
        ):
            break
    return chosen


def merge_refreshed_login(
    login: dict[str, Any], payload: dict[str, Any], now: datetime | None = None
) -> dict[str, Any]:
    """Replace only the rotated token fields. Other profile fields stay put."""

    updated = dict(login)
    access = str(payload.get("access_token") or "")
    if access:
        updated["key"] = access
    refresh = str(payload.get("refresh_token") or "")
    if refresh:
        updated["refresh_token"] = refresh
    expires_in = number(payload.get("expires_in"))
    if expires_in > 0:
        moment = now or datetime.now(timezone.utc)
        if moment.tzinfo is None:
            moment = moment.replace(tzinfo=timezone.utc)
        updated["expires_at"] = (
            (moment + timedelta(seconds=expires_in))
            .astimezone(timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%SZ")
        )
    return updated


def scan_opencode_messages(
    storage: Path, today: datetime | None = None
) -> dict[str, Any]:
    now = today or datetime.now().astimezone()
    today_key = now.strftime("%Y-%m-%d")
    stats = empty_stats(now)
    sessions: set[str] = set()
    today_sessions: set[str] = set()
    active_days: set[str] = set()
    message_root = storage / "message"
    if not message_root.is_dir():
        return finalize_stats(stats, sessions, today_sessions, active_days)
    for path in message_root.rglob("*.json"):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(payload, dict) or payload.get("role") not in (
            None,
            "assistant",
        ):
            continue
        provider = str(payload.get("providerID") or "")
        model = str(payload.get("modelID") or "opencode")
        if provider not in {"opencode", "opencode-go"} and not model.startswith(
            "opencode-go/"
        ):
            continue
        tokens = (
            payload.get("tokens")
            if isinstance(payload.get("tokens"), dict)
            else {}
        )
        cache = (
            tokens.get("cache") if isinstance(tokens.get("cache"), dict) else {}
        )
        created = None
        time_info = payload.get("time")
        if isinstance(time_info, dict):
            created = time_info.get("created")
        _add_model(
            stats,
            day=local_day(created, now),
            today=today_key,
            session_key=str(payload.get("sessionID") or path.parent.name),
            model=model or "opencode",
            input_tokens=number(tokens.get("input")),
            output_tokens=number(tokens.get("output"))
            + number(tokens.get("reasoning")),
            cache_read=number(cache.get("read")),
            cache_write=number(cache.get("write")),
            prompts=1,
            sessions=sessions,
            today_sessions=today_sessions,
            active_days=active_days,
        )
    return finalize_stats(stats, sessions, today_sessions, active_days)


def extract_opencode_key(auth: dict[str, Any]) -> str:
    for name in ("opencode-go", "opencode"):
        entry = auth.get(name)
        if isinstance(entry, dict) and entry.get("key"):
            return str(entry["key"])
        if isinstance(entry, str) and entry:
            return entry
    return ""


def parse_opencode_usage(
    payload: dict[str, Any],
) -> tuple[list[dict[str, str | float]], str]:
    usage = (
        payload.get("usage")
        if isinstance(payload.get("usage"), dict)
        else payload
    )
    if not isinstance(usage, dict):
        return [], ""
    windows = (
        ("rolling", "5h window", "Session"),
        ("weekly", "Weekly (7-day)", "Weekly"),
        ("monthly", "Monthly", "Monthly"),
    )
    limits = []
    for key, label, title in windows:
        window = usage.get(key)
        if not isinstance(window, dict):
            continue
        percent = fraction(window.get("percent", window.get("usagePercent")))
        if percent < 0:
            continue
        resets = str(window.get("resetsAt") or window.get("resetAt") or "")
        limits.append(
            {
                "label": label,
                "title": title,
                "percent": percent,
                "resetsAt": resets,
            }
        )
    plan = str(
        payload.get("plan")
        or payload.get("tier")
        or payload.get("subscription")
        or usage.get("plan")
        or ""
    )
    if not plan and limits:
        plan = "Go"
    if plan.lower() in {"plus", "go plus", "go-plus", "go_plus"}:
        plan = "Go Plus"
    elif plan.lower() == "go":
        plan = "Go"
    return limits, plan


def record(
    agent_id: str,
    name: str,
    stats: dict[str, Any],
    *,
    limits: list[dict[str, Any]] | None = None,
    tier: str = "",
    status: str = "",
    help_text: str = "",
    retry: bool = False,
) -> dict[str, Any]:
    payload = {
        "schemaVersion": 1,
        "id": agent_id,
        "name": name,
        "updatedAt": datetime.now(timezone.utc).isoformat(),
        "ready": True,
        "hasLocalStats": True,
        "tierLabel": tier,
        "usageStatusText": status,
        "authHelpText": help_text,
        "limits": limits or [],
    }
    payload.update(stats)
    if retry:
        payload["retryAdvised"] = True
    return payload


def _percent_text(value: float) -> str:
    percent = round(value * 100)
    return f"{percent}%"


def scan_pi_sessions(
    root: Path, provider: str, today: datetime | None = None
) -> dict[str, Any]:
    """Count assistant turns Pi recorded for one provider.

    Message text is ignored.
    """

    now = today or datetime.now().astimezone()
    today_key = now.strftime("%Y-%m-%d")
    stats = empty_stats(now)
    sessions: set[str] = set()
    today_sessions: set[str] = set()
    active_days: set[str] = set()
    if not root.is_dir():
        return finalize_stats(stats, sessions, today_sessions, active_days)
    for path in root.rglob("*.jsonl"):
        try:
            lines = path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
        except OSError:
            continue
        for line in lines:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            message = event.get("message") if isinstance(event, dict) else None
            if not isinstance(message, dict):
                continue
            if message.get("role") not in (None, "assistant"):
                continue
            if str(message.get("provider") or "") != provider:
                continue
            usage = (
                message.get("usage")
                if isinstance(message.get("usage"), dict)
                else {}
            )
            if not usage and not message.get("model"):
                continue
            _add_model(
                stats,
                day=local_day(
                    message.get("timestamp") or event.get("timestamp"), now
                ),
                today=today_key,
                session_key=path.stem,
                model=str(message.get("model") or provider),
                input_tokens=number(usage.get("input")),
                output_tokens=number(usage.get("output"))
                + number(usage.get("reasoning")),
                cache_read=number(usage.get("cacheRead")),
                cache_write=number(usage.get("cacheWrite")),
                prompts=1,
                sessions=sessions,
                today_sessions=today_sessions,
                active_days=active_days,
            )
    return finalize_stats(stats, sessions, today_sessions, active_days)


def iso_timestamp(value: Any) -> str:
    """Turn a Unix reset instant into a date the panel can parse."""

    if value is None or value == "":
        return ""
    if isinstance(value, str) and not value.strip().isdigit():
        return value
    try:
        seconds = float(value)
    except (TypeError, ValueError):
        return str(value)
    if seconds > 10_000_000_000:
        seconds /= 1000.0
    if seconds <= 0:
        return ""
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat()


def parse_codex_wham(
    payload: dict[str, Any], now: datetime | None = None
) -> tuple[list[dict[str, Any]], str, str]:
    plan = str(payload.get("plan_type") or "").replace("_", " ").strip()
    if plan:
        plan = plan.title()
    limit = (
        payload.get("rate_limit")
        if isinstance(payload.get("rate_limit"), dict)
        else {}
    )
    moment = now or datetime.now(timezone.utc)
    limits = []
    for key in ("primary_window", "secondary_window"):
        window = limit.get(key)
        if not isinstance(window, dict) or window.get("used_percent") is None:
            continue
        percent = fraction(window.get("used_percent"))
        if percent < 0:
            continue
        seconds = number(window.get("limit_window_seconds"))
        if seconds >= 6 * 86400:
            label, title = "Weekly (7-day)", "Weekly"
        elif seconds >= 3600:
            label, title = f"{max(1, round(seconds / 3600))}h window", "Session"
        else:
            label, title = "Limit", "Limit"
        resets = iso_timestamp(window.get("reset_at"))
        if not resets and window.get("reset_after_seconds") is not None:
            resets = (
                moment
                + timedelta(seconds=number(window.get("reset_after_seconds")))
            ).isoformat()
        limits.append(
            {
                "label": label,
                "title": title,
                "percent": percent,
                "resetsAt": str(resets),
            }
        )
    status = (
        "Rate limit reached"
        if limit.get("limit_reached") or limit.get("allowed") is False
        else ""
    )
    return limits, plan, status


def pi_token_current(
    entry: dict[str, Any], now_ms: int, skew_ms: int = 300_000
) -> bool:
    expires = number(entry.get("expires"))
    access = str(entry.get("access") or "")
    return bool(access) and expires > now_ms + skew_ms


def merge_pi_oauth(
    entry: dict[str, Any],
    payload: dict[str, Any],
    now_ms: int,
    skew_ms: int = 300_000,
) -> dict[str, Any]:
    updated = dict(entry)
    access = str(payload.get("access_token") or "")
    if access:
        updated["access"] = access
    refresh = str(payload.get("refresh_token") or "")
    if refresh:
        updated["refresh"] = refresh
    expires_in = number(payload.get("expires_in"))
    if expires_in > 0:
        updated["expires"] = now_ms + expires_in * 1000 - skew_ms
    updated["type"] = entry.get("type") or "oauth"
    return updated


def waybar_status(records: list[dict[str, Any]]) -> dict[str, str]:
    lines = []
    fullest = -1.0
    for item in records:
        if not isinstance(item, dict) or not item.get("id"):
            continue
        limits = (
            item.get("limits") if isinstance(item.get("limits"), list) else []
        )
        parts = [str(item.get("name") or item.get("id"))]
        tier = str(item.get("tierLabel") or "")
        if tier:
            parts.append(tier)
        for limit in limits:
            if not isinstance(limit, dict):
                continue
            percent = fraction(limit.get("percent"))
            if percent < 0:
                continue
            fullest = max(fullest, percent)
            title = str(limit.get("title") or limit.get("label") or "Limit")
            parts.append(f"{title} {_percent_text(percent)}")
        if not limits:
            note = str(
                item.get("usageStatusText") or item.get("authHelpText") or ""
            )
            if note:
                parts.append(note)
        lines.append(" · ".join(parts))
    tooltip = "Agents"
    if lines:
        tooltip = "Agents\n" + "\n".join(lines)
    else:
        tooltip = "Agents · collecting usage"
    css = "alarm" if fullest >= 0.9 else "ready" if lines else "idle"
    return {"text": "󱚣", "class": css, "tooltip": tooltip}
