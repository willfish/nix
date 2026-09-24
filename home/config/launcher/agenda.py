"""Read-only notes and a private Google Calendar iCal feed."""

import argparse
from datetime import date, datetime, time, timedelta, timezone
import json
import os
from pathlib import Path
import re
import resource
import stat
import subprocess
import sys
import tempfile
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from zoneinfo import ZoneInfo

import icalendar
import recurring_ical_events

MAX_BYTES = 2_000_000
MAX_COMPONENTS = 10_000
MAX_OCCURRENCES = 500
FETCH_TIMEOUT = 20
PARSE_TIMEOUT = 60
CPU_SECONDS = 30
ADDRESS_SPACE = 1024 * 1024 * 1024
FEED_URL = re.compile(
    r"^https://calendar\.google\.com/calendar/ical/"
    r"((?:[A-Za-z0-9._+-]|%[0-9A-Fa-f]{2})+)/"
    r"private-([A-Za-z0-9_-]{16,256})/basic\.ics$"
)
CALENDAR_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+@#-]*")


def local_zone():
    name = os.environ.get("TZ", "").lstrip(":")
    if name:
        try:
            return ZoneInfo(name)
        except (ValueError, KeyError):
            pass
    try:
        with open("/etc/localtime", "rb") as stream:
            return ZoneInfo.from_file(stream, key="local")
    except (OSError, ValueError):
        return datetime.now().astimezone().tzinfo


def clean(value):
    # Calendar and note text is untrusted terminal input, not markup or code.
    return "".join(
        c for c in str(value) if not unicodedata.category(c).startswith("C")
    )[:2000]


def load_json(path):
    with path.open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("file too large")
    return json.loads(raw)


def credentials_path():
    default = (
        Path(
            os.environ.get(
                "SOPS_NIX_SECRETS_DIR",
                str(
                    Path(
                        os.environ.get(
                            "XDG_CONFIG_HOME", str(Path.home() / ".config")
                        )
                    )
                    / "sops-nix/secrets"
                ),
            )
        )
        / "GOOGLE_CALENDAR_ICAL"
    )
    return Path(os.environ.get("DAILY_CALENDAR_CREDENTIALS", str(default)))


def feed_url(value):
    # Accept only the secret basic.ics address. The token stays in the path.
    if not isinstance(value, str) or not FEED_URL.fullmatch(value):
        raise ValueError("invalid calendar url")
    parts = urllib.parse.urlsplit(value)
    if (
        parts.netloc != "calendar.google.com"
        or parts.username
        or parts.password
        or parts.query
        or parts.fragment
        or parts.port not in (None, 443)
    ):
        raise ValueError("invalid calendar url")
    calendar_id = urllib.parse.unquote(FEED_URL.fullmatch(value).group(1))
    if (
        "%" in calendar_id
        or not CALENDAR_ID.fullmatch(calendar_id)
        or calendar_id in {".", ".."}
        or "/" in calendar_id
        or "\\" in calendar_id
    ):
        raise ValueError("invalid calendar url")
    return value


def private_token(url):
    return FEED_URL.fullmatch(url).group(2)


def credential_text(path):
    # sops-nix paths are symlinks; inspect the resolved target's permissions.
    info = path.stat()
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_mode & 0o077
        or info.st_size > 4096
    ):
        raise ValueError("credentials must be private")
    return path.read_text(encoding="utf-8").lstrip("\ufeff").strip()


def credentials(path):
    return feed_url(credential_text(path))


def calendar_feeds(path):
    text = credential_text(path)
    if not text.startswith("{"):
        return {"William": feed_url(text)}
    feeds = json.loads(text)
    if not isinstance(feeds, dict) or not 1 <= len(feeds) <= 8:
        raise ValueError("invalid calendar list")
    for name, url in feeds.items():
        if not name.strip() or len(name) > 40 or clean(name) != name:
            raise ValueError("invalid calendar name")
        feed_url(url)
    return feeds


def fetch_calendars(feeds, day, zone):
    events = []
    for name, url in feeds.items():
        for event in fetch_events(url, day, zone):
            events.append({**event, "calendar": name})
    if len(events) > MAX_OCCURRENCES:
        raise ValueError("too many events")
    events.sort(
        key=lambda event: (
            (0, event["start"]["date"])
            if "date" in event["start"]
            else (1, parse_stamp(event["start"]["dateTime"]).timestamp())
        )
    )
    return events


def cache_path():
    return (
        Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
        / "daily-agenda/calendar.json"
    )


def save_cache(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.parent.is_symlink() or path.is_symlink():
        raise ValueError("unsafe cache path")
    path.parent.chmod(0o700)
    fd, temporary = tempfile.mkstemp(prefix=".calendar-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # The private token is embedded in the URL; never forward it.
        return None


def read_limited(response):
    length = response.headers.get("Content-Length")
    if length is not None:
        try:
            declared = int(length)
        except (TypeError, ValueError):
            raise ValueError("invalid response") from None
        if declared < 0 or declared > MAX_BYTES:
            raise ValueError("response too large")
    raw = response.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("response too large")
    return raw


def open_feed(url):
    checked = feed_url(url)
    request = urllib.request.Request(
        checked,
        headers={
            "Accept": "text/calendar, text/plain",
            "Accept-Encoding": "identity",
        },
    )
    opener = urllib.request.build_opener(
        NoRedirect, urllib.request.ProxyHandler({})
    )
    try:
        with opener.open(request, timeout=FETCH_TIMEOUT) as response:
            if (
                getattr(response, "status", 200) != 200
                or response.geturl() != checked
            ):
                raise ValueError("calendar request failed")
            return read_limited(response)
    except urllib.error.URLError:
        raise ValueError("calendar request failed") from None


def parse_stamp(value):
    if not isinstance(value, str) or not value or len(value) > 64:
        raise ValueError("invalid event timestamp")
    stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if stamp.tzinfo is None or stamp.utcoffset() is None:
        raise ValueError("event lacks timezone")
    return stamp


def event_time(event, zone):
    start = event["start"]
    if not isinstance(start, dict):
        raise ValueError("invalid event start")
    if start.get("date"):
        if not isinstance(start["date"], str):
            raise ValueError("invalid event time")
        date.fromisoformat(start["date"])
        return "All day"
    return parse_stamp(start.get("dateTime")).astimezone(zone).strftime("%H:%M")


def validate_end(event):
    end = event.get("end")
    if end is None:
        return
    if not isinstance(end, dict):
        raise ValueError("invalid event time")
    start = event["start"]
    if "date" in end:
        if not isinstance(end["date"], str) or "date" not in start:
            raise ValueError("invalid event time")
        if date.fromisoformat(end["date"]) <= date.fromisoformat(start["date"]):
            raise ValueError("invalid event time")
    elif "dateTime" in end:
        if "dateTime" not in start or parse_stamp(
            end["dateTime"]
        ) < parse_stamp(start["dateTime"]):
            raise ValueError("invalid event time")
    else:
        raise ValueError("invalid event time")


def safe_link(value):
    link = clean(value or "")
    if not link.startswith("https://calendar.google.com/"):
        return ""
    lowered = link.casefold()
    if "private-" in lowered or "basic.ics" in lowered or "/ical/" in lowered:
        return ""
    return link


def redact(text, secrets):
    cleaned = clean(text)
    for secret in secrets:
        if secret and secret in cleaned:
            cleaned = cleaned.replace(secret, "[redacted]")
    return cleaned


def normalize_event(event, zone, secrets=()):
    if not isinstance(event, dict):
        raise ValueError("invalid event")
    summary = event.get("summary", "(Untitled event)")
    if not isinstance(summary, str):
        raise ValueError("invalid event")
    start = event.get("start")
    if not isinstance(start, dict):
        raise ValueError("invalid event")
    normalized = {
        "summary": redact(summary, secrets),
        "start": (
            {"date": start["date"]}
            if "date" in start
            else {"dateTime": start.get("dateTime")}
        ),
    }
    if "calendar" in event:
        name = event["calendar"]
        if not isinstance(name, str) or not name or len(name) > 40:
            raise ValueError("invalid calendar name")
        normalized["calendar"] = redact(name, secrets)
    end = event.get("end")
    if isinstance(end, dict) and "date" in end:
        normalized["end"] = {"date": end["date"]}
    elif isinstance(end, dict) and "dateTime" in end:
        normalized["end"] = {"dateTime": end["dateTime"]}
    elif end is not None:
        raise ValueError("invalid event time")
    link = safe_link(redact(event.get("htmlLink", ""), secrets))
    if link:
        normalized["htmlLink"] = link
    event_time(normalized, zone)
    validate_end(normalized)
    return normalized


def zone_name(zone):
    key = getattr(zone, "key", None)
    if not isinstance(key, str) or not key:
        raise ValueError("invalid timezone")
    return key


def attachment_zone(calendar, host_zone):
    name = str(calendar.get("X-WR-TIMEZONE") or "").strip()
    if not name:
        return host_zone
    try:
        zone = ZoneInfo(name)
    except (ValueError, KeyError):
        raise ValueError("invalid calendar timezone") from None
    return zone


def validate_components(raw, calendar):
    # Parser drops some bad EXDATE/RRULE values instead of raising.
    for event in calendar.walk("VEVENT"):
        if getattr(event, "errors", None):
            raise ValueError("invalid recurrence")
        start = event.get("DTSTART")
        if start is None or not isinstance(
            getattr(start, "dt", None), (date, datetime)
        ):
            raise ValueError("invalid event time")
        rule = event.get("RRULE")
        if rule is not None and not rule.get("FREQ"):
            raise ValueError("invalid recurrence")


def component_event(component, zone):
    status = str(component.get("STATUS") or "")
    if status.upper() == "CANCELLED":
        return None
    start = component.get("DTSTART")
    if start is None:
        raise ValueError("invalid event time")
    start_value = start.dt
    event = {"summary": clean(component.get("SUMMARY") or "(Untitled event)")}
    if isinstance(start_value, datetime):
        if start_value.tzinfo is None or start_value.utcoffset() is None:
            start_value = start_value.replace(tzinfo=zone)
        event["start"] = {"dateTime": start_value.isoformat()}
        end = component.get("DTEND")
        if end is not None and isinstance(end.dt, datetime):
            end_value = end.dt
            if end_value.tzinfo is None or end_value.utcoffset() is None:
                end_value = end_value.replace(tzinfo=zone)
            event["end"] = {"dateTime": end_value.isoformat()}
    elif isinstance(start_value, date):
        end = component.get("DTEND")
        end_value = (
            end.dt if end is not None else start_value + timedelta(days=1)
        )
        if isinstance(end_value, datetime) or end_value <= start_value:
            raise ValueError("invalid event time")
        event["start"] = {"date": start_value.isoformat()}
        event["end"] = {"date": end_value.isoformat()}
    else:
        raise ValueError("invalid event time")
    link = safe_link(component.get("URL"))
    if link:
        event["htmlLink"] = link
    return event


def day_window(day, zone):
    start = datetime.combine(day, time.min, zone)
    end = datetime.combine(day + timedelta(days=1), time.min, zone)
    span = end.astimezone(timezone.utc) - start.astimezone(timezone.utc)
    if span <= timedelta(0) or span > timedelta(hours=36):
        raise ValueError("invalid window")
    return start, end


def parse_feed(raw, start, end, zone=None):
    host = zone or start.tzinfo
    if getattr(host, "key", None) is None:
        raise ValueError("invalid timezone")
    if not isinstance(raw, (bytes, bytearray)) or len(raw) > MAX_BYTES:
        raise ValueError("response too large")
    if raw.upper().count(b"BEGIN:VEVENT") > MAX_COMPONENTS:
        raise ValueError("too many events")
    if b"BEGIN:VCALENDAR" not in raw.upper():
        raise ValueError("invalid calendar")
    try:
        calendar = icalendar.Calendar.from_ical(bytes(raw))
        if getattr(calendar, "name", None) != "VCALENDAR":
            raise ValueError("invalid calendar")
        validate_components(raw, calendar)
        attach = attachment_zone(calendar, host)
        occurrences = recurring_ical_events.of(calendar).between(start, end)
    except ValueError:
        raise
    except Exception:
        raise ValueError("invalid calendar") from None
    events, seen = [], set()
    for component in occurrences:
        event = component_event(component, attach)
        if event is None:
            continue
        identity = (
            str(component.get("UID") or ""),
            event["start"].get("date") or event["start"].get("dateTime"),
        )
        if identity in seen:
            continue
        seen.add(identity)
        events.append(event)
        if len(events) > MAX_OCCURRENCES:
            raise ValueError("too many events")
    events.sort(
        key=lambda item: (
            0 if "date" in item["start"] else 1,
            item["start"].get("date") or item["start"]["dateTime"],
            item["summary"],
        )
    )
    return events


def restrict_child():
    # Applied after fork and inherited across exec. Bounds calendar bombs.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_CPU, (CPU_SECONDS, CPU_SECONDS))
    for name in ("RLIMIT_AS", "RLIMIT_DATA"):
        limit = getattr(resource, name, None)
        if limit is None:
            continue
        try:
            resource.setrlimit(limit, (ADDRESS_SPACE, ADDRESS_SPACE))
        except (OSError, ValueError):
            if name == "RLIMIT_AS":
                raise
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_BYTES, MAX_BYTES))


def interpreter_env(zone):
    # Keep the wrapper's import path for the unwrapped Nix Python.
    paths = []
    for entry in list(sys.path) + os.environ.get("PYTHONPATH", "").split(":"):
        if (
            entry
            and entry not in paths
            and not entry.startswith(sys.base_prefix)
        ):
            paths.append(entry)
    return {
        "PATH": os.environ.get("PATH", ""),
        "PYTHONPATH": ":".join(paths),
        "PYTHONNOUSERSITE": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "TZ": zone_name(zone),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
    }


def expand_feed(raw, day, zone):
    command = [
        sys.executable,
        "-c",
        (
            "import runpy,sys; sys.argv=sys.argv[1:];"
            " runpy.run_path(sys.argv[0], run_name='__main__')"
        ),
        str(Path(__file__).resolve()),
        "--parse-feed",
        day.isoformat(),
        zone_name(zone),
    ]
    env = interpreter_env(zone)
    try:
        completed = subprocess.run(
            command,
            input=raw,
            capture_output=True,
            timeout=PARSE_TIMEOUT,
            preexec_fn=restrict_child,
            env=env,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise ValueError("calendar parse timed out") from None
    except (OSError, subprocess.SubprocessError):
        raise ValueError("invalid calendar") from None
    if completed.returncode != 0 or len(completed.stdout) > MAX_BYTES:
        raise ValueError("invalid calendar")
    try:
        events = json.loads(completed.stdout)
    except (json.JSONDecodeError, UnicodeError):
        raise ValueError("invalid calendar") from None
    if not isinstance(events, list) or len(events) > MAX_OCCURRENCES:
        raise ValueError("invalid calendar")
    return [normalize_event(event, zone) for event in events]


def fetch_events(url, day, zone):
    events = expand_feed(open_feed(url), day, zone)
    secrets = (
        url,
        private_token(url),
        urllib.parse.quote(private_token(url), safe=""),
    )
    return [normalize_event(event, zone, secrets) for event in events]


def reminders(path):
    if not path.exists():
        return [], "Today's notes file is missing."
    with path.open("r") as stream:
        text = stream.read(MAX_BYTES + 1)
    if len(text) > MAX_BYTES:
        raise ValueError("notes too large")
    rows, in_section, in_fence = [], False, False
    sections = {
        "reminders",
        "what i plan to do today",
        "today",
        "todos",
        "to do",
    }
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        bullet = re.match(r"^\s*[-*+]\s+(.*)$", line)
        checkbox = (
            re.match(r"^\[([ xX])\]\s*(.*)$", bullet.group(1))
            if bullet
            else None
        )
        if checkbox:
            if checkbox.group(1) == " " and checkbox.group(2).strip():
                rows.append(clean(checkbox.group(2)))
        elif bullet and in_section:
            rows.append(clean(bullet.group(1)))
        elif stripped:
            heading = stripped.lstrip("#").strip().strip("*:").casefold()
            # Plain standup and Markdown headings delimit lists.
            in_section = heading in sections
    return list(dict.fromkeys(rows)), None


def cached_events(path, day, now):
    if not path.exists():
        return [], "Calendar not cached yet."
    value = load_json(path)
    if not isinstance(value, dict) or value.get("day") != day.isoformat():
        return [], "No calendar cache for today."
    fetched = parse_stamp(value.get("fetched_at"))
    if now.tzinfo is not None:
        age = (now - fetched.astimezone(now.tzinfo)).total_seconds()
    else:
        age = (now - fetched).total_seconds()
    events = value.get("events")
    if not isinstance(events, list) or len(events) > MAX_OCCURRENCES:
        raise ValueError("invalid cache")
    checked = []
    for event in events:
        if (
            not isinstance(event, dict)
            or not isinstance(event.get("summary", ""), str)
            or len(event.get("summary", "")) > 4000
        ):
            raise ValueError("invalid cache")
        checked.append(normalize_event(event, now.tzinfo or fetched.tzinfo))
    return (
        checked,
        ("Calendar cache is stale. " if age > 1800 or age < 0 else "")
        + "Last refreshed "
        + fetched.astimezone(now.tzinfo or fetched.tzinfo).strftime("%H:%M %Z")
        + ".",
    )


def parse_feed_cli(argv):
    try:
        if len(argv) != 2:
            return 1
        day = date.fromisoformat(argv[0])
        zone = local_zone() if argv[1] == "local" else ZoneInfo(argv[1])
        if zone.key != argv[1]:
            return 1
        start, end = day_window(day, zone)
        raw = sys.stdin.buffer.read(MAX_BYTES + 1)
        json.dump(parse_feed(raw, start, end, zone), sys.stdout)
        return 0
    except Exception:
        return 1


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv[:1] == ["--parse-feed"]:
        return parse_feed_cli(argv[1:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--status", action="store_true")
    args = parser.parse_args(argv)
    now = datetime.now(local_zone())
    day, cache, secret_path = now.date(), cache_path(), credentials_path()
    warnings = []
    configured = secret_path.exists()
    if args.status:
        if not configured:
            print("Calendar feed: missing")
        else:
            try:
                feeds = calendar_feeds(secret_path)
                print("Calendar feed: present")
                print("Calendars: " + ", ".join(feeds))
            except (OSError, ValueError, UnicodeError):
                print("Calendar feed: unusable")
        try:
            _, status = cached_events(cache, day, now)
            print(status)
        except (OSError, ValueError, KeyError, TypeError):
            print("Calendar cache unavailable or invalid.")
        return 0
    if args.refresh:
        try:
            events = fetch_calendars(
                calendar_feeds(secret_path), day, now.tzinfo
            )
            save_cache(
                cache,
                {
                    "day": day.isoformat(),
                    "fetched_at": now.isoformat(),
                    "events": events,
                },
            )
        except (
            OSError,
            ValueError,
            KeyError,
            TypeError,
            urllib.error.URLError,
            subprocess.SubprocessError,
        ):
            warnings.append(
                "Calendar refresh failed; check access, credentials and"
                " network. Using cache if available."
            )
    if not configured:
        warnings.append("Google Calendar access is not configured.")
    pretty = sys.stdout.isatty()
    print(
        ("\n  \033[1;36mTODAY\033[0m  " + now.strftime("%A, %-d %B") + "\n")
        if pretty
        else "Today's agenda: " + day.isoformat()
    )
    print("  \033[1mREMINDERS\033[0m\n" if pretty else "\nNotes")
    try:
        rows, warning = reminders(
            Path.home() / "Notes" / day.isoformat() / "today.md"
        )
        if warning:
            warnings.append(warning)
        for row in rows:
            import textwrap

            print(
                textwrap.fill(
                    row,
                    width=70,
                    initial_indent="  ◦ " if pretty else "  [Notes] ",
                    subsequent_indent="    ",
                )
            )
        if not rows:
            print(
                "  Nothing on your list."
                if pretty
                else "  No unfinished reminders found."
            )
    except (OSError, ValueError):
        warnings.append("Today's notes could not be read.")
    print("\n  \033[1mSCHEDULE\033[0m\n" if pretty else "\nGoogle Calendar")
    try:
        events, status = cached_events(cache, day, now)
        if not pretty:
            print("  " + status)
        for event in events:
            print(
                (
                    "  \033[36m" + event_time(event, now.tzinfo) + "\033[0m  "
                    if pretty
                    else "  [Calendar] " + event_time(event, now.tzinfo) + " "
                )
                + clean(event.get("summary", "(Untitled event)"))
                + (
                    ("  · " + event["calendar"])
                    if event.get("calendar")
                    else ""
                )
            )
            link = safe_link(event.get("htmlLink", ""))
            if link and not pretty:
                print("    " + link)
        if (
            not events
            and cache.exists()
            and status.startswith("Last refreshed")
        ):
            print("  No events today.")
    except (OSError, ValueError, KeyError, TypeError):
        warnings.append("Calendar cache unavailable or invalid.")
    if pretty:
        message = (
            "Calendar temporarily unavailable · showing saved events"
            if any("Calendar" in w or "calendar" in w for w in warnings)
            else status
        )
        print("\n  \033[2m" + message + "\033[0m")
    else:
        for warning in warnings:
            print("\nWarning: " + warning)
    return 1 if args.refresh and warnings else 0


if __name__ == "__main__":
    raise SystemExit(main())
