"""On-demand PersonaPlex browser mode, mutually exclusive with Pi voice."""

import subprocess
import time
import urllib.error
import urllib.request

URL = "http://127.0.0.1:8998"


class Conversation:
    def __init__(self, run=subprocess.run):
        self.run = run

    def systemctl(self, *args):
        return self.run(
            ["systemctl", "--user", *args],
            check=True,
            capture_output=True,
            text=True,
            timeout=20,
        ).stdout.strip()

    def active(self):
        return self.systemctl(
            "show", "personaplex.service", "--property=ActiveState", "--value"
        ) in (
            "active",
            "activating",
            "deactivating",
        )

    def start(self):
        self.systemctl("start", "--no-block", "personaplex.service")
        self.open()

    def open(self):
        self.systemctl("restart", "--no-block", "personaplex-open.service")

    def stop(self):
        self.systemctl(
            "stop", "personaplex-open.service", "personaplex.service"
        )
        self.systemctl("start", "pi-voice.service", "pi-voice-osd.service")

    def menu(self, picker):
        rows = [
            ("open", "Open PersonaPlex conversation (experimental)"),
            ("pi", "Switch back to Pi dictation and playback"),
        ]
        action = picker("Voice mode: PersonaPlex | separate from Pi", rows)
        if action == "pi":
            self.stop()
        elif action == "open":
            self.open()
        elif action is not None:
            raise RuntimeError("Invalid conversation action")


def can_switch(status):
    return (
        status.get("phase", "idle") == "idle"
        and not status.get("retained")
        and not status.get("preparing_transcription")
        and not status.get("retry")
        and not status.get("pending")
        and not status.get("draft")
    )


def open_when_ready(conversation=None, timeout=240):
    conversation = conversation or Conversation()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not conversation.active():
            raise RuntimeError(
                "PersonaPlex stopped or failed; check its service journal"
            )
        try:
            with urllib.request.urlopen(URL, timeout=2) as response:
                if (
                    response.status == 200
                    and b"<title>PersonaPlex</title>" in response.read(16384)
                ):
                    subprocess.run(["xdg-open", URL], check=True, timeout=15)
                    return
        except (urllib.error.URLError, TimeoutError):
            pass
        time.sleep(1)
    raise RuntimeError("PersonaPlex did not become ready within four minutes")


if __name__ == "__main__":
    from voice_controller import desktop_notice

    try:
        open_when_ready()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        desktop_notice("PersonaPlex unavailable", str(exc))
        raise SystemExit(1)
