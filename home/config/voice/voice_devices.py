"""Cached, read-only PipeWire microphone discovery for voice recording."""

import json
import subprocess
import threading
import time


class MicrophoneMonitor:
    def __init__(self, preferred=None, refresh_seconds=5,
                 runner=subprocess.run):
        self.preferred = preferred or None
        self.refresh_seconds = refresh_seconds
        self.runner = runner
        self._lock = threading.Lock()
        self._snapshot = self._unknown()
        self._updated = float("-inf")
        self._probe = None

    def status(self):
        """Return cached metadata while one bounded worker refreshes it."""
        with self._lock:
            if time.monotonic() - self._updated >= self.refresh_seconds:
                self._start_probe()
            return dict(self._snapshot)

    def resolve(self):
        """Refresh before capture; callers must use the recording worker."""
        with self._lock:
            done = self._start_probe()
        if not done.wait(2.2):
            return self._unknown("Microphone details unavailable")
        with self._lock:
            return dict(self._snapshot)

    def _start_probe(self):
        if self._probe is None:
            done = threading.Event()
            self._probe = done

            def refresh():
                result = self._read()
                with self._lock:
                    self._snapshot = result
                    self._updated = time.monotonic()
                    self._probe = None
                    done.set()

            threading.Thread(
                target=refresh, name="voice-microphone-probe", daemon=True
            ).start()
        return self._probe

    def _unknown(self, error=None):
        return {
            "name": "System default", "target": None, "muted": None,
            "preferred": self.preferred, "missing": bool(self.preferred),
            "error": error,
        }

    def _read(self):
        try:
            response = self.runner(
                ["pw-dump"], stdin=subprocess.DEVNULL, capture_output=True,
                text=True, timeout=2, check=True,
            )
            return self._parse(json.loads(response.stdout))
        except (OSError, subprocess.SubprocessError, ValueError,
                TypeError, AttributeError):
            return self._unknown("Microphone details unavailable")

    def _parse(self, rows):
        sources, default = {}, None
        for row in rows:
            info = row.get("info") or {}
            props = info.get("props") or {}
            if props.get("media.class") == "Audio/Source":
                sources[props.get("node.name")] = info
            if row.get("props", {}).get("metadata.name") == "default":
                for entry in row.get("metadata", []):
                    if entry.get("key") == "default.audio.source":
                        value = entry.get("value") or {}
                        if isinstance(value, str):
                            value = json.loads(value)
                        default = value.get("name")
        chosen = self.preferred if self.preferred in sources else default
        if chosen not in sources:
            return self._unknown("No default microphone available")
        info = sources[chosen]
        props = info["props"]
        settings = info.get("params", {}).get("Props", [])
        muted = next((item["mute"] for item in settings if "mute" in item),
                     None)
        return {
            "name": props.get("node.description")
            or props.get("node.nick") or chosen,
            "target": chosen if chosen == self.preferred else None,
            "muted": muted, "preferred": self.preferred,
            "missing": bool(self.preferred and chosen != self.preferred),
            "error": None,
        }
