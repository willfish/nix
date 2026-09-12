#!/usr/bin/env python3
"""Local speech and hotkeys for one explicitly selected Codex pane."""

import array
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
import io
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import socketserver
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
import wave

# Also support direct loading by the isolated regression harness.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from voice_errors import DeliveryUncertain
from voice_audio import LocalAudio
from voice_labels import SnapshotCache, SocketKey, build_labels
from voice_engines import EngineManager
from voice_harness import PiTerminal
from dataclasses import asdict
from pi_attachments import (
    AttachmentRegistry,
    PaneKey,
    RetainedDictation,
    LEASE_SECONDS,
    MAX_RETAINED_BYTES,
    bridge_identity,
    pane_key,
    can_auto_select,
    event_matches,
    socket_instance,
    validate_endpoint,
)


def dictation_text(text):
    """Whisper's non-speech markers are metadata, never prompt contents."""
    if not isinstance(text, str):
        raise RuntimeError("Whisper returned an invalid transcript")
    text = re.sub(
        r"[\[(]\s*(?:blank[_ ]audio|no[_ ]speech|silence|silent|music|"
        r"inaudible|noise|applause|laughter|breathing)\s*[\])]",
        " ", text, flags=re.I,
    )
    text = text.replace("\u200b", "").replace("\ufeff", "")
    text = re.sub(r"[ \t]+", " ", text).strip()
    return text if any(c.isalnum() for c in text) else ""


def has_control_characters(text):
    return any(
        (ord(c) < 32 and c not in "\n\r\t") or 127 <= ord(c) < 160
        for c in text
    )


def has_audio(path):
    """Discard quiet recordings before Whisper can hallucinate speech."""
    with wave.open(str(path), "rb") as wav:
        if (
            wav.getnchannels() != 1
            or wav.getsampwidth() != 2
            or wav.getframerate() != 16000
        ):
            raise RuntimeError("Expected 16 kHz mono PCM recording")
        samples = array.array("h", wav.readframes(wav.getnframes()))
    if len(samples) < 3200:
        return False
    voiced = sum(
        1
        for i in range(0, len(samples), 320)
        if math.sqrt(sum(v * v for v in samples[i : i + 320]) / 320) >= 200
    )
    return voiced >= 8


def spoken_text(text):
    """Extract a bounded final summary, never fall back to the full reply."""
    if not isinstance(text, str):
        return ""
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line)
        if fence:
            if (marker and marker[1][0] == fence[0]
                    and len(marker[1]) >= len(fence) and not marker[2].strip()):
                fence = None
            continue
        if marker:
            fence = marker[1]
            continue
        lines.append(line)

    start, inline = None, ""
    for index, line in enumerate(lines):
        label = re.fullmatch(
            r" {0,3}(?:#{1,6}[ \t]+)?(?:Spoken summary|Summary|TL;DR|TLDR)"
            r"(?::[ \t]*(.*)|[ \t]*)",
            line.replace("**", ""), flags=re.I,
        )
        if label:
            start, inline = index + 1, label[1] or ""
    if start is None:
        return ""
    tail = lines[start:]
    # The summary must be the final section, not an overview before details.
    if any(re.match(r"^ {0,3}(?:#{1,6}(?:\s|$)|(?:=+|-+)\s*$)", line)
           for line in tail):
        return ""
    text = "\n".join([inline, *tail])
    text = re.sub(r"!?\[([^\]]+)\]\([^\n)]*\)", r"\1", text)
    text = re.sub(r"^\s*(?:[-*+]\s+|\d+[.)]\s+|>\s*)", "", text, flags=re.M)
    text = text.replace("**", "").replace("__", "").replace("`", "")
    text = " ".join(text.split())
    if (len(text) > 1500 or len(text.split()) > 120
            or not any(c.isalnum() for c in text)):
        return ""
    return text


def process_start(pid):
    """Linux process start ticks prevent a reused PID from becoming a target."""
    try:
        return (
            Path(f"/proc/{int(pid)}/stat")
            .read_text()
            .rsplit(")", 1)[1]
            .split()[19]
        )
    except (OSError, ValueError, IndexError):
        return None


def is_cli_thread(thread_id, codex_home=None):
    """Reject inherited subagent callbacks using installed Codex metadata.

    This is deliberately a read-only, fail-closed adapter for schema version 5.
    No transcript contents or unrelated thread records are read.
    """
    home = Path(
        codex_home or os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))
    )
    try:
        uri = (home / "state_5.sqlite").resolve().as_uri() + "?mode=ro"
        with closing(sqlite3.connect(uri, uri=True, timeout=1)) as db:
            row = db.execute(
                "SELECT source FROM threads WHERE id = ?", (thread_id,)
            ).fetchone()
        return row == ("cli",)
    except (OSError, sqlite3.Error):
        return False


class Herdr:
    def input_request(self, target, text):
        # pane.send_text is raw bytes. send_input observes bracketed-paste mode.
        request = {
            "id": uuid.uuid4().hex,
            "method": "pane.send_input",
            "params": {"pane_id": target["pane"], "text": text, "keys": []},
        }
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(8)
            try:
                sock.connect(target["socket"])
            except OSError as exc:
                raise RuntimeError(
                    "Selected Herdr pane is unavailable"
                ) from exc
            try:
                sock.sendall(json.dumps(request).encode() + b"\n")
                with sock.makefile("rb") as incoming:
                    response = json.loads(incoming.readline(1024 * 1024))
                if response.get("id") != request["id"]:
                    raise ValueError("Mismatched response")
            except (OSError, ValueError) as exc:
                raise DeliveryUncertain(
                    "Could not confirm paste; check the selected Codex prompt "
                    "and send it there if the dictation arrived"
                ) from exc
        if "error" in response:
            raise RuntimeError("Could not paste into the selected Codex pane")

    def request(self, target, *args):
        env = dict(os.environ, HERDR_SOCKET_PATH=target["socket"])
        try:
            result = subprocess.run(
                ["herdr", *args],
                env=env,
                capture_output=True,
                text=True,
                timeout=1 if target.get('managed') else 8,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(
                "Could not reach the selected Herdr pane"
            ) from exc
        if result.returncode:
            raise RuntimeError("Could not reach the selected Herdr pane")
        try:
            return json.loads(result.stdout)["result"]
        except (ValueError, KeyError, TypeError) as exc:
            raise RuntimeError("Invalid response from the Herdr pane") from exc

    def validate_target(self, target):
        harness = target.get("harness", "codex")
        if (
            not target.get("start")
            or process_start(target["pid"]) != target["start"]
        ):
            raise RuntimeError(
                "Selected agent has exited; start its voice launcher again"
            )
        info = self.request(
            target, "pane", "process-info", "--pane", target["pane"]
        )["process_info"]
        procs = info.get("foreground_processes", [])
        if not any(
            p["pid"] == target["pid"] and harness in p.get("name", "")
            for p in procs
        ):
            raise RuntimeError(
                "The selected agent process is no longer in the foreground"
            )
        agent = self.request(target, "agent", "get", target["pane"])["agent"]
        if agent.get("agent") != harness:
            raise RuntimeError(f"The selected pane is not running {harness}")
        return agent

    def validate(self, target):
        agent = self.validate_target(target)
        state = agent.get("agent_status")
        if state not in ("idle", "done"):
            raise RuntimeError(
                f"Selected agent is {state or 'not ready'}; "
                "finish its current interaction first"
            )

    def insert(self, target, text):
        self.validate(target)
        self.input_request(target, text)

    def submit(self, target):
        self.validate(target)
        # Herdr adds its own blocked-agent and foreground checks here.
        self.request(target, "agent", "prompt", target["pane"], " ")

    def insert_guarded(self, target, text, cancelled):
        self.validate(target)
        if cancelled.is_set():
            raise RuntimeError("Voice delivery was cancelled")
        self.input_request(target, text)

    def submit_guarded(self, target, cancelled, allow_edited=False):
        self.validate(target)
        if cancelled.is_set():
            raise RuntimeError("Voice delivery was cancelled")
        self.request(target, "agent", "prompt", target["pane"], " ")


class AgentTerminal:
    def __init__(self):
        self.herdr, self.pi = Herdr(), PiTerminal()

    def _adapter(self, target):
        if (not target.get("start") or process_start(target["pid"])
                != target["start"]):
            raise RuntimeError("Selected voice process has exited")
        return self.pi if target.get("harness") in ("pi", "qwen-pi") \
            else self.herdr

    def validate_target(self, target):
        return self._adapter(target).validate_target(target)

    def validate(self, target):
        return self._adapter(target).validate(target)

    def activity(self, target):
        info = self._adapter(target).validate_target(target)
        state = info.get("agent_status", info.get("state", "unknown"))
        if state == "done":
            return "idle"
        return state if state in ("working", "blocked", "idle") else "unknown"

    def insert_guarded(self, target, text, cancelled):
        return self._adapter(target).insert_guarded(target, text, cancelled)

    def submit_guarded(self, target, cancelled, allow_edited=False):
        return self._adapter(target).submit_guarded(
            target, cancelled, allow_edited=allow_edited
        )


class Controller:
    def __init__(self, runtime, terminal, audio, notice, *,
                 catalogue=None, engines=None):
        self.runtime = runtime
        self.terminal, self.audio = terminal, audio
        self._notice = notice
        self.lock = threading.RLock()
        self.delivery_lock = threading.Lock()
        self.input_cancelled = threading.Event()
        self.target = None
        self.token = None
        self.sessions = {}
        self.draft = False
        self.cancelled = threading.Event()
        self.reply = None
        self.thread = None
        self.turns = set()
        self.auto = False
        self.playback = None
        self.capture = None
        self.record_cancelled = threading.Event()
        self.phase = "idle"
        self.error = None
        self.pending = None
        self.record_started = None
        self.worker = None
        self.retry_audio = None
        self.active_retry = None
        self.activity_inflight = set()
        self.attachments = AttachmentRegistry()
        self.selection_initialized = False
        self.selection_explicit = False
        self.reconnect_pane = None
        self.reconnect_explicit = False
        self.retained_dictation = None
        self.recovery_revision = 0
        self.recovery_inflight = False
        self.recovery_uncertain = False
        self.pending_admissions = 0
        self.attachments_closed = False
        self.catalogue = catalogue
        self.engines = engines
        self.retry_lease = None
        self.show_team = False
        self.recording_label = None

    def _population(self):
        if self.engines:
            self.engines.set_population(
                len(self.sessions), self.pending_admissions
            )

    def refresh_labels(self):
        if not self.catalogue:
            return
        with self.lock:
            targets = [
                dict(entry['target']) for entry in self.sessions.values()
            ]
            reconnect = self.reconnect_pane
        keys = set()
        if reconnect:
            keys.add(SocketKey(
                reconnect.socket_path, reconnect.socket_device,
                reconnect.socket_inode,
            ))
        for target in targets:
            if target.get('harness') not in ('pi', 'qwen-pi'):
                continue
            try:
                if target.get('managed'):
                    pane = pane_key(target)
                    key = SocketKey(
                        pane.socket_path, pane.socket_device, pane.socket_inode
                    )
                else:
                    key = SocketKey(*socket_instance(target['socket']))
                keys.add(key)
                with self.lock:
                    entry = self.sessions.get(target.get('token'))
                    if entry and entry['target'] == target:
                        entry['socket_key'] = key
            except (OSError, ValueError, KeyError):
                continue
        # The catalogue is bounded; overflow rows retain their pane labels.
        keys = set(sorted(keys)[:self.catalogue.max_keys])
        self.catalogue.set_active(keys)
        for key in keys:
            self.catalogue.refresh(key)

    @property
    def transcribing(self):
        return self.phase == "transcribing"

    def notice(self, title, detail=""):
        def deliver():
            try:
                self._notice(title, detail)
            except Exception:
                pass

        threading.Thread(target=deliver, daemon=True).start()

    def recording_active(self):
        return self.phase in (
            "starting", "recording", "stopping", "transcribing"
        )

    def _cue(self, frequency):
        # A disconnected speaker or failed notification must not stop capture.
        def play():
            try:
                self.audio.cue(frequency)
            except Exception:
                print("Codex voice: sound cue unavailable", file=sys.stderr)

        threading.Thread(target=play, daemon=True).start()

    def report_error(self, error):
        with self.lock:
            self.error = str(error)
            if not self.recording_active():
                self.phase = "error"

    def _validate_attachment(self, incoming):
        """Validate endpoint and foreground pane identity outside the lock."""
        if not isinstance(incoming, dict) or incoming.get('harness') not in (
            'pi',
            'qwen-pi',
        ):
            raise ValueError('Only Pi sessions support managed attachment')
        for key in ('pane', 'socket', 'session', 'bridge_id', 'adapter_socket'):
            value = incoming.get(key)
            if (
                not isinstance(value, str)
                or not value
                or len(value) > 4096
                or has_control_characters(value)
            ):
                raise ValueError('Invalid Pi attachment identity')
        pid, activation = incoming.get('pid'), incoming.get('activation')
        if (
            type(pid) is not int
            or pid <= 0
            or type(activation) is not int
            or not 0 < activation <= 2**53 - 1
            or type(incoming.get('team_child', False)) is not bool
        ):
            raise ValueError('Invalid Pi process or activation')
        start = process_start(pid)
        if not start or Path(f'/proc/{pid}').stat().st_uid != os.getuid():
            raise ValueError('Pi process is unavailable')
        endpoint = validate_endpoint(
            self.runtime, incoming['adapter_socket'], pid, incoming['bridge_id']
        )
        instance = socket_instance(incoming['socket'])
        target = {
            key: incoming.get(key)
            for key in (
                'pane',
                'pid',
                'harness',
                'adapter_socket',
                'session',
                'bridge_id',
                'activation',
                'model',
                'thinking',
            )
        }
        target.update(
            socket=instance[0],
            socket_device=instance[1],
            socket_inode=instance[2],
            start=start,
            managed=True,
            team_child=incoming.get('team_child', False),
            adapter_device=endpoint[0],
            adapter_inode=endpoint[1],
        )
        info = Herdr().request(
            target, 'pane', 'process-info', '--pane', target['pane']
        )['process_info']
        if not isinstance(info, dict) or info.get('pane_id') != target['pane']:
            raise ValueError('Herdr did not confirm the full Pi pane identity')
        foreground = info.get('foreground_processes', [])
        if not isinstance(foreground, list) or not all(
            isinstance(p, dict) for p in foreground
        ):
            raise ValueError('Invalid Pi foreground process information')
        process = next((p for p in foreground if p.get('pid') == pid), None)
        name = process.get('name', '') if process else ''
        name = name.lower() if isinstance(name, str) else ''
        if name in ('node', 'nodejs'):
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            is_pi = any(
                arg.endswith(
                    (
                        b'/pi',
                        b'/pi/dist/cli.js',
                        b'/pi-coding-agent/dist/cli.js',
                    )
                )
                for arg in argv
            )
        else:
            is_pi = name in ('pi', 'qwen-pi')
        if not is_pi:
            raise ValueError(
                'Pi process is no longer in the claimed foreground pane'
            )
        # Neither client start ticks nor an observation preceding pane I/O is
        # trusted.
        if (
            process_start(pid) != start
            or socket_instance(target['socket']) != instance
            or validate_endpoint(
                self.runtime, target['adapter_socket'], pid, target['bridge_id']
            )
            != endpoint
        ):
            raise ValueError('Pi identity changed during attachment')
        return target

    def attach(self, incoming):
        """Quiet admission, without probing Pi before it receives its token."""
        with self.lock:
            self.pending_admissions += 1
            self._population()
        try:
            # A revision mismatch requires new foreground evidence, not a blind
            # write.
            for _ in range(3):
                try:
                    address = str(Path(incoming['socket']).resolve())
                    slot = (address, incoming['pane'])
                    with self.lock:
                        revision = self.attachments.revisions.get(slot, 0)
                    target = self._validate_attachment(incoming)
                    identity = bridge_identity(target)
                    if process_start(target['pid']) != target['start']:
                        return {'state': 'connecting'}
                    with self.lock:
                        if self.attachments_closed:
                            return {'state': 'superseded'}
                        admission = self.attachments.admit(
                            identity, target['activation'], revision
                        )
                        if admission.state == 'retry':
                            continue
                        if admission.state == 'superseded':
                            return {'state': 'superseded'}
                        token = admission.token
                        if admission.state == 'existing':
                            entry = self.sessions[token]
                            # Same bridge UUID cannot mutate conversation or
                            # endpoint.
                            if any(
                                entry['target'].get(key) != target.get(key)
                                for key in (
                                    'session',
                                    'adapter_socket',
                                    'adapter_device',
                                    'adapter_inode',
                                    'activation',
                                    'harness',
                                    'team_child',
                                )
                            ):
                                return {'state': 'invalid'}
                            return {
                                'token': token,
                                'state': entry['connection_state'],
                            }
                        selected = (
                            admission.replaced == self.token
                            and self.token is not None
                            and pane_key(self.target) == identity.pane
                        )
                        explicit = self.selection_explicit
                        if admission.replaced:
                            self._remove_session(
                                admission.replaced, retire=True
                            )
                        # Reconcile an explicit launcher entry for this same
                        # process/pane.
                        for old_token, entry in list(self.sessions.items()):
                            old = entry['target']
                            if (
                                not old.get('managed')
                                and old.get('harness') in ('pi', 'qwen-pi')
                                and all(
                                    old.get(k) == target.get(k)
                                    for k in ('pane', 'socket', 'pid', 'start')
                                )
                            ):
                                selected = selected or old_token == self.token
                                explicit = explicit or (
                                    old_token == self.token
                                    and self.selection_explicit
                                )
                                self._remove_session(old_token)
                        if (
                            self.reconnect_pane
                            and self.reconnect_pane.socket_path
                            == identity.pane.socket_path
                            and (
                                self.reconnect_pane.socket_device,
                                self.reconnect_pane.socket_inode,
                            )
                            != (
                                identity.pane.socket_device,
                                identity.pane.socket_inode,
                            )
                        ):
                            self.reconnect_pane = None
                            self.reconnect_explicit = False
                        target['token'] = token
                        self.sessions[token] = self._new_entry(target)
                        self._population()
                        if selected:
                            self._load_selected(token, explicit=explicit)
                            self.reconnect_pane = identity.pane
                            self.reconnect_explicit = explicit
                        self._save_selection()
                        return {'token': token, 'state': 'connecting'}
                except (ValueError, RuntimeError, OSError, KeyError, TypeError):
                    return {'state': 'connecting'}
            return {'state': 'connecting'}
        finally:
            with self.lock:
                self.pending_admissions -= 1
                self._population()

    @staticmethod
    def _new_entry(target):
        session = target.get('session')
        return dict(
            target=target,
            thread=session,
            candidate=session,
            turns=set(),
            pending=None,
            draft=False,
            reply=None,
            connection_state='connecting',
            heartbeat_at=time.monotonic(),
            agent_state='unknown',
        )

    def _load_selected(self, token, *, explicit=False):
        entry = self.sessions[token]
        self.token, self.target = token, entry['target']
        self.thread, self.turns = entry['thread'], set(entry['turns'])
        self.pending, self.draft, self.reply = (
            entry['pending'],
            entry['draft'],
            entry['reply'],
        )
        self.input_cancelled = threading.Event()
        self.cancelled = threading.Event()
        self.record_cancelled = threading.Event()
        self.selection_explicit = explicit
        self.phase = 'draft' if self.pending or self.draft else 'idle'
        self.error = None

    def _require_ready(self, token):
        entry = self.sessions.get(token)
        if entry and entry['target'].get('managed'):
            if (
                entry.get('connection_state') != 'ready'
                or time.monotonic() - entry.get('heartbeat_at', 0)
                >= LEASE_SECONDS
            ):
                raise RuntimeError('Pi voice is reconnecting; wait for Ready')

    def _remove_session(self, token, *, retire=True):
        entry = self.sessions.get(token)
        if not entry:
            return
        selected = token == self.token
        if selected:
            self._remember()
        target = entry['target']
        if target.get('harness') in ('pi', 'qwen-pi'):
            self.retain_dictation(entry.get('pending'), token, target)
        if selected:
            if target.get('managed'):
                self.reconnect_pane = pane_key(target)
                self.reconnect_explicit = self.selection_explicit
            if self.recording_active():
                self.record_cancelled.voice_target_lost = True
                self.notice(
                    'Recording cancelled', 'Its voice destination disconnected'
                )
            self.stop(target_lost=True)
            self.token, self.target = None, None
            self.thread, self.reply, self.pending = None, None, None
            self.turns, self.draft = set(), False
            self.phase = 'idle'
        self.sessions.pop(token, None)
        self.attachments.remove(token, retire=retire)
        self._population()

    def detach(self, token, identity):
        with self.lock:
            entry = self.sessions.get(token)
            if (
                not entry
                or not entry['target'].get('managed')
                or not event_matches(entry['target'], identity, session=False)
            ):
                return False
            self._remove_session(token)
            self._save_selection()
            return True

    def monitor_attachments(self, now=None):
        """One testable monitor tick, with pane/process I/O outside the lock."""
        now = time.monotonic() if now is None else now
        with self.lock:
            entries = list(self.sessions.items())
            captured = set(self.attachments.highwater) | {
                item[:2] for item in self.attachments.retired
            }
            fence_pids = {pid for pid, _start in captured}
        starts = {pid: process_start(pid) for pid in fence_pids}
        failures = []
        for token, entry in entries:
            target = entry['target']
            dead = process_start(target['pid']) != target.get('start')
            if target.get('managed'):
                expired = now - entry.get('heartbeat_at', 0) >= LEASE_SECONDS
                if not dead and not expired:
                    try:
                        validated = self._validate_attachment(target)
                        dead = any(
                            validated[k] != target[k]
                            for k in (
                                'start',
                                'socket_device',
                                'socket_inode',
                                'adapter_device',
                                'adapter_inode',
                            )
                        )
                    except (
                        ValueError,
                        RuntimeError,
                        OSError,
                        KeyError,
                        TypeError,
                    ):
                        dead = True
                if expired or dead:
                    failures.append((token, entry, dead))
            elif dead:
                failures.append((token, entry, True))
        removed = []
        with self.lock:
            for token, entry, dead in failures:
                if self.sessions.get(token) is not entry:
                    continue
                # A heartbeat during unlocked validation wins over lease expiry.
                if (
                    not dead
                    and now - entry.get('heartbeat_at', 0) < LEASE_SECONDS
                ):
                    continue
                self._remove_session(token, retire=False)
                removed.append(token)
            self.attachments.prune(starts, captured)
            self._expire_retry()
            if removed:
                self._save_selection()
        self.refresh_reconnect()
        self.refresh_labels()
        if self.engines:
            self.engines.sweep()
        return removed

    def refresh_reconnect(self):
        """Forget a disconnected destination only with fresh evidence."""
        with self.lock:
            pane = self.reconnect_pane
        if not pane:
            return
        try:
            instance = socket_instance(pane.socket_path)
            if instance[1:] != (pane.socket_device, pane.socket_inode):
                self.reconcile_panes(*instance, set())
                return
            if self.catalogue:
                state = self.catalogue.read(SocketKey(*instance))
                if state.authoritative(self.catalogue.clock()):
                    self.reconcile_panes(*instance, set(state.panes))
                return
            target = dict(socket=pane.socket_path, managed=True)
            snapshot = Herdr().request(target, 'api', 'snapshot')['snapshot']
            panes = snapshot['panes']
            if (
                not isinstance(panes, list)
                or not all(
                    isinstance(row, dict)
                    and isinstance(row.get('pane_id'), str)
                    for row in panes
                )
                or socket_instance(pane.socket_path) != instance
            ):
                return
            self.reconcile_panes(*instance, {row['pane_id'] for row in panes})
        except (OSError, ValueError, RuntimeError, KeyError, TypeError):
            pass

    def reconcile_panes(self, socket_path, device, inode, pane_ids):
        """Apply only a fresh, authoritative pane catalogue."""
        with self.lock:
            pane = self.reconnect_pane
            if (
                pane
                and pane.socket_path == socket_path
                and (
                    (pane.socket_device, pane.socket_inode) != (device, inode)
                    or pane.pane_id not in pane_ids
                )
            ):
                self.reconnect_pane = None
                self.reconnect_explicit = False
                self._save_selection()

    def retain_dictation(self, text, token, target, cancelled=None):
        with self.lock:
            if (
                self.retained_dictation is not None
                or not isinstance(text, str)
                or not text
                or target.get('harness') not in ('pi', 'qwen-pi')
                or has_control_characters(text)
                or len(text.encode('utf-8')) > MAX_RETAINED_BYTES
                or (
                    cancelled
                    and (
                        getattr(
                            cancelled,
                            'recovery_revision',
                            self.recovery_revision,
                        )
                        != self.recovery_revision
                        or (
                            cancelled.is_set()
                            and not getattr(
                                cancelled, 'voice_target_lost', False
                            )
                        )
                    )
                )
            ):
                return False
            self.recovery_uncertain = False
            self.retained_dictation = RetainedDictation(
                text,
                token,
                target.get('session') or '',
                target.get('harness', 'pi') + ': ' + target['pane'],
            )
            return True

    def recover_copy(self):
        with self.lock:
            retained = self.retained_dictation
        if not retained:
            raise RuntimeError('No retained dictation')
        try:
            subprocess.run(
                ['wl-copy'],
                input=retained.text,
                text=True,
                check=True,
                timeout=5,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise RuntimeError(
                'Could not copy retained dictation; try again'
            ) from exc

    def recover_discard(self):
        with self.lock:
            if self.recovery_inflight:
                raise RuntimeError(
                    'Wait for retained dictation staging to finish'
                )
            self.retained_dictation = None
            self.recovery_uncertain = False
            self.recovery_revision += 1
            # An explicit discard also prevents a cancelled late transcript
            # resurfacing.
            self.record_cancelled.voice_target_lost = False

    def recover_stage(self):
        with self.lock:
            retained = self.retained_dictation
            if not retained or self.recovery_inflight:
                raise RuntimeError('No retained dictation available to stage')
            if self.recovery_uncertain:
                raise RuntimeError(
                    'Check the Pi prompt; copy or discard '
                    'this uncertain dictation'
                )
            if (
                not self.target
                or self.target.get('harness') not in ('pi', 'qwen-pi')
                or not self.selection_explicit
            ):
                raise RuntimeError(
                    'Explicitly select a ready Pi session for recovery'
                )
            if self.recording_active() or self.pending or self.draft:
                raise RuntimeError('Finish or discard the selected draft first')
            self._require_ready(self.token)
            token = self.token
            self.input_cancelled = threading.Event()
            cancelled = self.input_cancelled
            self.recovery_inflight = True
        try:
            staged = self.stage(retained.text, token, cancelled)
            with self.lock:
                accepted = getattr(
                    cancelled, 'delivery_outcome', None) == 'accepted'
                if (staged or accepted) and self.retained_dictation is retained:
                    self.retained_dictation = None
            return staged
        except DeliveryUncertain:
            with self.lock:
                self.recovery_uncertain = True
            raise
        finally:
            with self.lock:
                self.recovery_inflight = False

    def register(self, token, target, thread=None, turns=()):
        with self.lock:
            lost = bool(self.target and self.target.get('managed'))
            if target.get('harness') in ('pi', 'qwen-pi'):
                for existing_token, entry in list(self.sessions.items()):
                    existing = entry['target']
                    if (
                        existing.get('managed')
                        and existing.get('pane') == target.get('pane')
                        and existing.get('socket') == target.get('socket')
                    ):
                        self._remove_session(existing_token)
            self._remember()
            if lost and self.target:
                if self.retain_dictation(self.pending, self.token, self.target):
                    self.sessions[self.token]['pending'] = None
                self.record_cancelled.voice_target_lost = True
            self.stop(target_lost=lost)
            target = dict(target, token=token)
            target.setdefault("harness", "codex")
            target["session"] = thread
            self.sessions[token] = {
                "target": target, "thread": thread, "turns": set(turns),
                "candidate": thread, "pending": None, "draft": False,
                "reply": None,
            }
            self._population()
            self.selection_initialized = True
            self.reconnect_pane = None
            self._load_selected(token, explicit=True)
            self._save_selection()

    def _remember(self):
        if self.token in self.sessions:
            self.sessions[self.token].update(
                thread=self.thread, turns=self.turns, pending=self.pending,
                draft=self.draft, reply=self.reply,
            )

    def select(self, token):
        with self.lock:
            entry = self.sessions.get(token)
            if not entry:
                raise RuntimeError('That voice session is no longer available')
            target = dict(entry['target'])
        if target.get('managed'):
            validated = self._validate_attachment(target)
            if validated != {k: v for k, v in target.items() if k != 'token'}:
                raise RuntimeError('That Pi voice session changed')
        with self.lock:
            if self.sessions.get(token) is not entry:
                raise RuntimeError('That voice session is no longer available')
            self._remember()
            if (
                self.token != token
                and self.target
                and self.target.get('harness') in ('pi', 'qwen-pi')
            ):
                if self.retain_dictation(self.pending, self.token, self.target):
                    self.sessions[self.token]['pending'] = None
                if self.recording_active():
                    self.record_cancelled.voice_target_lost = True
            self.stop(target_lost=True)
            self.selection_initialized = True
            self.reconnect_pane = None
            self._load_selected(token, explicit=True)
            self._save_selection()

    def rebind(self):
        with self.lock:
            if not self.target:
                raise RuntimeError("Select a voice session first")
            self.stop()
            candidate = self.sessions[self.token].get("candidate")
            if self.target["harness"] == "codex" and candidate == self.thread:
                excluded = self.sessions[self.token].setdefault("excluded", [])
                if self.thread:
                    excluded.append(self.thread)
                candidate = None
            self.thread = candidate
            self.target = dict(self.target, session=candidate)
            self.sessions[self.token]["target"] = self.target
            self._set_activity(self.sessions[self.token], "unknown")
            self.turns = set()
            self.draft = False
            self.reply = None
            self.phase = "idle"
            self.input_cancelled = threading.Event()
            self._save_selection()
        self.notice(
            "Voice rebound", "Using the selected session's conversation"
        )

    def _save_selection(self):
        self._remember()
        path = self.runtime / "selection.tmp"
        path.write_text(
            json.dumps(
                {
                    "token": self.token,
                    "show_team": self.show_team,
                    "selection_initialized": self.selection_initialized,
                    "selection_explicit": self.selection_explicit,
                    "reconnect_pane": (
                        asdict(self.reconnect_pane)
                        if self.reconnect_pane
                        else None
                    ),
                    "reconnect_explicit": self.reconnect_explicit,
                    "attachment_fences": self.attachments.fences(),
                    "target": self.target,
                    "thread": self.thread,
                    "turns": sorted(self.turns),
                    "sessions": {
                        token: {
                            "target": entry["target"],
                            "thread": entry["thread"],
                            "candidate": entry.get("candidate"),
                            "excluded": entry.get("excluded", []),
                            "turns": sorted(entry["turns"]),
                        }
                        for token, entry in self.sessions.items()
                    },
                }
            )
        )
        path.chmod(0o600)
        path.replace(self.runtime / "selection.json")

    def restore(self):
        with self.lock:
            self.target, self.token = None, None
            try:
                saved = json.loads(
                    (self.runtime / "selection.json").read_text()
                )
                target = saved["target"]
                self.show_team = bool(saved.get('show_team', False))
                self.selection_initialized = saved.get(
                    'selection_initialized', bool(saved.get('token') or target)
                )
                self.selection_explicit = saved.get(
                    'selection_explicit', bool(saved.get('token'))
                )
                self.reconnect_pane = (
                    PaneKey(**saved['reconnect_pane'])
                    if saved.get('reconnect_pane')
                    else None
                )
                self.reconnect_explicit = saved.get('reconnect_explicit', False)
                self.attachments = AttachmentRegistry()
                self.attachments.restore_fences(
                    saved.get('attachment_fences', {})
                )
                if target and target.get('managed'):
                    self.reconnect_pane = pane_key(target)
                    self.reconnect_explicit = self.selection_explicit
                self.sessions = {}
                for token, entry in saved.get("sessions", {}).items():
                    existing = entry["target"]
                    if (existing.get("start") and process_start(
                        existing["pid"]
                    ) == existing["start"]):
                        if existing.get('managed'):
                            admission = self.attachments.admit(
                                bridge_identity(existing),
                                existing['activation'],
                                self.attachments.revision(pane_key(existing)),
                                token=token,
                            )
                            if admission.state != 'admitted':
                                continue
                            self.sessions[token] = self._new_entry(existing)
                            self._population()
                        else:
                            self.sessions[token] = dict(
                                entry,
                                turns=set(entry.get('turns', [])),
                                pending=None,
                                draft=False,
                                reply=None,
                            )
                            self._population()
                if (
                    not target or not target.get("start")
                    or process_start(target["pid"]) != target["start"]
                ):
                    return False
                if saved["token"] in self.sessions:
                    self._load_selected(
                        saved['token'], explicit=self.selection_explicit
                    )
                elif target.get('managed'):
                    return False
                else:
                    self.register(
                        saved["token"], target, saved.get("thread"),
                        saved.get("turns", []),
                    )
                return True
            except (OSError, ValueError, KeyError):
                return False
            finally:
                self._population()

    def unregister(self, token):
        with self.lock:
            self._remove_session(token)
            self._save_selection()

    @staticmethod
    def _set_activity(entry, state):
        entry["agent_state"] = state
        entry["activity_revision"] = entry.get("activity_revision", 0) + 1

    def _settle_activity(self, entry, turn=None):
        active = entry.get("active_turn")
        if turn and active and turn != active:
            return
        if turn and (
            turn == active or entry["target"].get("harness", "codex") == "codex"
        ):
            self._set_activity(entry, "idle")
            return
        # Pi has no shared run ID. Such events
        # cannot establish ordering relative to a newer turn or status probe.
        self._set_activity(entry, entry.get("agent_state", "unknown"))
        entry["activity_checked"] = float("-inf")

    def _refresh_activity(self):
        activity = getattr(self.terminal, "activity", None)
        entry = self.sessions.get(self.token)
        if not activity or not entry or self.token in self.activity_inflight:
            return
        now = time.monotonic()
        if now - entry.get("activity_checked", float("-inf")) < 1:
            return
        token, target = self.token, dict(self.target)
        revision = entry.get("activity_revision", 0)
        entry["activity_checked"] = now
        self.activity_inflight.add(token)

        def probe():
            try:
                state = activity(target)
            except Exception:
                state = "unknown"
            if state not in ("working", "blocked", "idle"):
                state = "unknown"
            with self.lock:
                if (self.sessions.get(token) is entry
                        and entry["target"] == target
                        and entry.get("activity_revision", 0) == revision):
                    self._set_activity(entry, state)
                self.activity_inflight.discard(token)

        threading.Thread(target=probe, daemon=True).start()

    def status(self):
        audio_status = getattr(self.audio, "status", lambda: {})()
        with self.lock:
            self._refresh_activity()
            agent_state = self.sessions.get(self.token, {}).get(
                "agent_state", "unknown"
            )
            result = {
                "show_team": self.show_team,
                "selection_explicit": self.selection_explicit,
                "recording_label": self.recording_label,
                "pane": self.target["pane"] if self.target else None,
                "connection_state": self.sessions.get(self.token, {}).get(
                    'connection_state',
                    (
                        'ready'
                        if self.target
                        else (
                            'reconnecting'
                            if self.reconnect_pane
                            else 'unselected'
                        )
                    ),
                ),
                "retained": self.retained_dictation is not None,
                "retained_source": (
                    self.retained_dictation.source_label
                    if self.retained_dictation
                    else None
                ),
                "harness": (
                    self.target.get("harness", "codex") if self.target else None
                ),
                "agent_state": agent_state,
                "responding": agent_state == "working",
                "session_label": (self.thread or "Awaiting conversation")[:48],
                "rebind_needed": bool(
                    self.token
                    and self.sessions[self.token].get("candidate")
                    not in (
                        [self.thread]
                        + self.sessions[self.token].get("excluded", [])
                    )
                ),
                "sessions": [
                    {
                        "token": token,
                        "pane": entry['target']['pane'],
                        "socket_key": (
                            SocketKey(
                                entry['target']['socket'],
                                entry['target']['socket_device'],
                                entry['target']['socket_inode'],
                            )
                            if entry['target'].get('managed')
                            else entry.get('socket_key')
                        ),
                        "model": entry['target'].get('model'),
                        "thinking": entry['target'].get('thinking'),
                        "selected": token == self.token,
                        "harness": entry["target"].get("harness", "codex"),
                        "team_child": entry['target'].get('team_child', False),
                        "connection_state": entry.get(
                            'connection_state', 'ready'
                        ),
                        "id": entry.get("thread") or entry["target"]["pane"],
                        "label": (
                            entry["target"].get("harness", "codex")
                            + ": "
                            + (
                                entry['target']['pane']
                                if entry['target'].get('harness')
                                in ('pi', 'qwen-pi')
                                else (
                                    entry.get('thread')
                                    or entry['target']['pane']
                                )
                            )
                        ),
                    }
                    for token, entry in self.sessions.items()
                    if (
                        self.show_team
                        or not entry['target'].get('team_child')
                        or token == self.token
                    )
                ],
                "microphone": {
                    **audio_status.get("microphone", {}),
                    "clipping": getattr(self.capture, "clipping", False),
                },
                "models": (
                    None
                    if not self.sessions
                    else (
                        "unavailable"
                        if "error" in audio_status.get("backends", {}).values()
                        else (
                            "ready"
                            if audio_status.get("backends")
                            and all(
                                v == "ready"
                                for v in audio_status["backends"].values()
                            )
                            else "loading" if self.sessions else None
                        )
                    )
                ),
                "model_error": "; ".join(
                    audio_status.get("backend_errors", {}).values()
                ),
                "draft": self.draft,
                "reply": self.reply,
                "auto": self.auto,
                "can_speak": bool(
                    self.target and not self.target.get('team_child')
                ),
                "selected_voice": audio_status.get(
                    "selected_voice", "samantha"
                ),
                "voices": audio_status.get("voices", {}),
                "recording": self.phase == "recording",
                "transcribing": self.transcribing,
                "speaking": bool(self.playback and self.playback.is_alive()),
                "phase": self.phase,
                "error": self.error,
                "pending": bool(self.pending),
                "retry": self.retry_audio is not None,
                "recording_seconds": (
                    max(0, time.monotonic() - self.record_started)
                    if self.record_started and self.phase == "recording"
                    else 0
                ),
                "input_level": getattr(self.capture, "level", 0.0),
            }

        snapshots = self.catalogue.snapshots() if self.catalogue else {}
        rows = result['sessions']
        labelled = {row['token']: row for row in build_labels(
            [row for row in rows if row['socket_key'] is not None], snapshots)}
        result['sessions'] = [labelled.get(row['token'], row) for row in rows]
        for row in result['sessions']:
            row.pop('socket_key', None)
        return result

    def notify(self, token, event):
        with self.lock:
            entry = self.sessions.get(token)
            if entry and event.get("type") == "agent-turn-complete":
                if event.get("thread-id") in entry.get("excluded", []):
                    return False
                entry["candidate"] = event.get("thread-id")
                if token != self.token:
                    thread = event.get("thread-id")
                    if not entry["thread"] or entry["thread"] == thread:
                        if event.get("turn-id") not in entry["turns"]:
                            self._settle_activity(entry, event.get("turn-id"))
                        entry["thread"] = thread
                        entry["draft"] = False
                        entry["reply"] = spoken_text(event.get(
                            "last-assistant-message"
                        ) or "")
                        entry["turns"].add(event.get("turn-id", ""))
                        self._save_selection()
                    return False
            if (
                not self.target
                or token != self.token
                or event.get("type") != "agent-turn-complete"
            ):
                return False
            thread, turn = event.get("thread-id"), event.get("turn-id")
            if (
                not thread
                or not turn
                or (self.thread and self.thread != thread)
            ):
                return False
            if turn in self.turns:
                return False
            self._settle_activity(entry, turn)
            self.thread = thread
            self.turns.add(turn)
            self._save_selection()
            self.draft = False
            if self.phase == "draft" and not self.pending:
                self.phase = "idle"
            self.reply = spoken_text(event.get("last-assistant-message") or "")
            if (
                self.auto
                and not self.target.get('team_child')
                and self.reply
                and not self.recording_active()
                and not self.pending
            ):
                self.read(replace=True)
            return True

    def harness_event(self, token, event):
        if not isinstance(event, dict):
            return False
        with self.lock:
            entry = self.sessions.get(token)
            if entry and entry['target'].get('managed'):
                target = dict(entry['target'])
                if not event_matches(target, event):
                    return False
            else:
                target = None
        if target:
            kind = event.get('type')
            if kind in ('ready', 'heartbeat'):
                try:
                    validated = self._validate_attachment(target)
                    if any(
                        validated[k] != target[k]
                        for k in (
                            'start',
                            'socket_device',
                            'socket_inode',
                            'adapter_device',
                            'adapter_inode',
                        )
                    ):
                        return False
                except (ValueError, RuntimeError, OSError, KeyError, TypeError):
                    return False
            with self.lock:
                if self.sessions.get(token) is not entry:
                    return False
                if kind in ('ready', 'heartbeat'):
                    if (
                        kind == 'heartbeat'
                        and entry.get('connection_state') != 'ready'
                    ):
                        return False
                    entry['heartbeat_at'] = time.monotonic()
                    if kind == 'ready':
                        entry['connection_state'] = 'ready'
                        reconnect = self.reconnect_pane == pane_key(target)
                        if (
                            self.token is None and reconnect
                        ) or can_auto_select(
                            selection_initialized=self.selection_initialized,
                            selected_token=self.token,
                            ready=True,
                            team_child=target.get('team_child', False),
                        ):
                            self._load_selected(
                                token,
                                explicit=(
                                    self.reconnect_explicit
                                    if reconnect
                                    else False
                                ),
                            )
                            self.selection_initialized = True
                        if self.token == token:
                            self.reconnect_pane = None
                        self._save_selection()
                    return True
                if kind == 'metadata':
                    for key in ('model', 'thinking'):
                        if key in event:
                            entry['target'][key] = event[key]
                    return True
                if kind == 'shutdown':
                    self._remove_session(token)
                    self._save_selection()
                    return True
                if entry.get('connection_state') != 'ready':
                    return False
        with self.lock:
            entry = self.sessions.get(token)
            if (not entry or event.get("harness") != entry["target"].get(
                "harness"
            ) or not event.get("session")):
                return False
            if event.get("pid") and event["pid"] != entry["target"]["pid"]:
                return False
            session, kind = event["session"], event.get("type")
            if kind == "session":
                entry["candidate"] = session
                if not entry["thread"]:
                    entry["thread"] = session
                    entry["target"]["session"] = session
                    if token == self.token:
                        self.thread = session
                self._save_selection()
                return True
            if entry["thread"] != session:
                return False
            if kind == "busy":
                self._set_activity(entry,
                    "blocked" if event.get("state") == "blocked" else "working"
                )
                entry["draft"] = False
                entry["active_turn"] = event.get("turn")
                if token == self.token:
                    self.draft = False
                    if self.phase == "draft" and not self.pending:
                        self.phase = "idle"
                return True
            if kind in ("settled", "shutdown"):
                self._settle_activity(entry, event.get("turn"))
                return True
            if kind != "reply" or not event.get("turn"):
                return False
            target = dict(entry["target"])
        if event.get("provisional"):
            def complete():
                # Provisional replies may continue the turn. Wait for Herdr's
                # authoritative ready state without blocking the hook process.
                for _ in range(40):
                    with self.lock:
                        current = self.sessions.get(token)
                        if (not current or current["thread"] != session
                                or current.get("active_turn") not in (
                                    None, event["turn"]
                                )):
                            return
                    try:
                        self.terminal.validate(target)
                    except RuntimeError:
                        time.sleep(0.25)
                        continue
                    self.notify(token, {
                        "type": "agent-turn-complete", "thread-id": session,
                        "turn-id": event["turn"],
                        "last-assistant-message": event.get("text", ""),
                    })
                    return

            threading.Thread(target=complete, daemon=True).start()
            return True
        notified = self.notify(token, {
            "type": "agent-turn-complete", "thread-id": session,
            "turn-id": event["turn"],
            "last-assistant-message": event.get("text", ""),
        })
        return True if target.get('managed') else notified

    def read(self, replace=False):
        with self.lock:
            if self.target and self.target.get('team_child'):
                raise RuntimeError('Team members are silent')
            if self.pending:
                raise RuntimeError("Send or cancel retained dictation first")
            if self.recording_active():
                raise RuntimeError(
                    "Finish recording and transcription before reading a reply"
                )
            playing = bool(self.playback and self.playback.is_alive())
            self.stop()
            if playing and not replace:
                return
            if not self.reply:
                raise RuntimeError(
                    "No summary in the latest completed reply"
                )
            self.cancelled = threading.Event()
            self.playback = threading.Thread(
                target=self._speak,
                args=(self.reply, self.cancelled),
                daemon=True,
            )
            self.playback.start()

    @staticmethod
    def _await_engine(lease, cancelled):
        while lease and not cancelled.is_set():
            try:
                lease.wait(timeout=.1)
                break
            except TimeoutError:
                if lease.ready.done():
                    raise
        return not cancelled.is_set()

    def _speak(self, text, cancelled):
        lease = None
        try:
            lease = self.engines.acquire('tts') if self.engines else None
            if self._await_engine(lease, cancelled):
                self.audio.speak(text, cancelled)
        except Exception as exc:
            if not cancelled.is_set():
                self.notice("Could not read the reply", str(exc))
        finally:
            if lease:
                lease.release()

    def interact(self):
        with self.lock:
            recording = self.recording_active() or not (
                self.draft or self.pending
            )
            if self.input_cancelled.is_set():
                self.input_cancelled = threading.Event()
            expected = (self.token, self.input_cancelled)
        if recording:
            self.record()
        else:
            self.send(expected=expected, allow_edited=True)

    def record(self, mode="append"):
        labels = {
            row['token']: row.get('full_label', row['label'])
            for row in self.status()['sessions']
        }
        with self.lock:
            if self.retained_dictation is not None:
                raise RuntimeError(
                    'Resolve retained dictation before recording again'
                )
            self._require_ready(self.token)
            if self.phase == "starting":
                self.stop()
                self.notice("Recording cancelled", "Microphone was starting")
                return
            if self.phase == "recording":
                self.phase = "stopping"
                threading.Thread(
                    target=self.capture.close, daemon=True
                ).start()
                return
            if self.phase == "stopping":
                return
            if self.transcribing:
                raise RuntimeError(
                    "Still transcribing; wait for the dictation to appear"
                )
            if not self.target:
                raise RuntimeError("Start codex-voice in a Herdr pane first")
            previous = self.pending
            self.stop(discard=False)
            self.draft = False
            self.pending = previous
            self.error = None
            self.phase = "starting"
            self.recording_label = labels.get(self.token, self.target['pane'])
            self.record_cancelled = threading.Event()
            self.record_cancelled.recovery_revision = self.recovery_revision
            self.input_cancelled = threading.Event()
            path = self.runtime / (uuid.uuid4().hex + ".wav")
            self.worker = threading.Thread(
                target=self._record,
                args=(path, self.token, self.target, self.record_cancelled,
                      previous, mode),
                daemon=True,
            )
            self.worker.start()

    def _record(self, path, token, target, cancelled, previous=None,
                mode="append", retry=False, deadline=None):
        capture = None
        lease = getattr(cancelled, 'engine_lease', None)
        recovered = False

        def recover(text, *, combined=False, drained=False):
            nonlocal recovered
            # A draining HTTP worker must not prolong shutdown waiting for the
            # controller, nor acquire this lock while holding an audio lock.
            while not self.attachments_closed:
                if self.lock.acquire(timeout=.05):
                    break
            else:
                return
            try:
                if (
                    recovered
                    or self.attachments_closed
                    or (
                        drained
                        and not getattr(cancelled, 'voice_target_lost', False)
                    )
                ):
                    return
                text = dictation_text(text)
                if text and previous and mode == 'append' and not combined:
                    text = previous + '\n' + text
                if not text:
                    return
                recovered = True
                retained = self.retained_dictation
                # Session removal may already have saved the pre-recording
                # draft.
                # Only this same operation may extend it with drained new words.
                extend = (
                    previous
                    and mode == 'append'
                    and retained
                    and retained.source_token == token
                    and retained.text == previous
                    and getattr(cancelled, 'recovery_revision', -1)
                    == self.recovery_revision
                    and getattr(cancelled, 'voice_target_lost', False)
                )
                if extend:
                    self.retained_dictation = None
                if (
                    not self.retain_dictation(text, token, target, cancelled)
                    and extend
                ):
                    self.retained_dictation = retained
            finally:
                self.lock.release()

        try:
            if lease is None and self.engines:
                lease = self.engines.acquire('stt')
            # Readiness can be transiently unknown while Codex redraws. Check
            # identity now and enforce idle/done only when delivering text.
            self._probe_managed(target)
            self.terminal.validate_target(target)
            if cancelled.is_set():
                return
            if not retry:
                capture = self.audio.start_capture(path)
                with self.lock:
                    if cancelled.is_set():
                        return
                    self.capture = capture
                if not capture.wait_ready(timeout=5):
                    raise RuntimeError("Microphone did not become ready")
                with self.lock:
                    if cancelled.is_set():
                        return
                    self.record_started = time.monotonic()
                    self.phase = "recording"
                self._cue(880)
                self.notice(
                    "Recording", "Press Super+Space to stop; maximum 3 minutes"
                )
                code = capture.wait(timeout=185)
                if code not in (0, -signal.SIGINT) or getattr(
                    capture, "error", None
                ):
                    raise RuntimeError(
                        getattr(capture, "error", None)
                        or "Microphone capture failed"
                    )
            with self.lock:
                if cancelled.is_set():
                    return
                self.capture = None
                self.phase = "transcribing"
            self._cue(660)
            if not has_audio(path):
                text = ""
            else:
                try:
                    if not self._await_engine(lease, cancelled):
                        return
                    transcribe_drained = getattr(
                        self.audio, 'transcribe_drained', None)
                    if transcribe_drained is not None:
                        result = transcribe_drained(
                            path,
                            cancelled,
                            lambda text: recover(text, drained=True),
                        )
                    else:
                        result = self.audio.transcribe(path, cancelled)
                    text = dictation_text(result)
                except Exception:
                    with self.lock:
                        if not cancelled.is_set() and token == self.token:
                            self.retry_lease = lease
                            lease = None
                            self.retry_audio = (
                                path, token, target,
                                deadline or time.monotonic() + 120,
                                previous, mode,
                            )
                    raise
            with self.lock:
                if cancelled.is_set() or token != self.token:
                    recover(text)
                    return
            if not text and previous:
                with self.lock:
                    if cancelled.is_set() or token != self.token:
                        return
                    self.pending = previous
                    self.phase = "draft"
                self.notice("No new speech", "Previous dictation retained")
                return
            if text and previous and mode == "append":
                text = previous + "\n" + text
            try:
                staged = self.stage(text, token, cancelled)
            except DeliveryUncertain:
                with self.lock:
                    # A lost acknowledgement is different from a failed
                    # connection. Never paste or press Enter again blindly.
                    if token == self.token and not cancelled.is_set():
                        self.pending = None
                        self.draft = False
                raise
            except RuntimeError as exc:
                with self.lock:
                    if cancelled.is_set() or token != self.token:
                        recover(text, combined=True)
                        return
                    # Keep a valid transcript when the pane is temporarily
                    # busy/unreachable. Never paste it into another session.
                    if text and not has_control_characters(text):
                        self.pending = text
                        self.phase = "draft"
                        self.notice(
                            "Dictation retained",
                            f"{exc}. Press Send when the agent is ready.",
                        )
                        return
                raise
            if (
                not staged
                and text
                and getattr(cancelled, 'delivery_outcome', None) != 'accepted'
            ):
                with self.lock:
                    if token != self.token or cancelled.is_set():
                        recover(text, combined=True)
                        return
            if staged:
                self.notice(
                    "Dictation ready", "Press Super+Space again to send"
                )
            else:
                self.notice("No speech detected", "Nothing was inserted")
        except Exception as exc:
            with self.lock:
                if (
                    not cancelled.is_set()
                    and self.record_cancelled is cancelled
                ):
                    self.error = str(exc)
                    self.phase = "error"
                    self.notice("Dictation stopped", str(exc))
        finally:
            try:
                if capture:
                    capture.close()
            finally:
                with self.lock:
                    if lease and lease is not self.retry_lease:
                        lease.release()
                    if self.active_retry and self.active_retry[2] is cancelled:
                        self.active_retry = None
                    if not self.retry_audio or self.retry_audio[0] != path:
                        path.unlink(missing_ok=True)
                    if self.record_cancelled is cancelled:
                        self.capture = None
                        self.record_started = None
                        if self.recording_active():
                            self.phase = "draft" if self.pending else "idle"

    def _expire_retry(self, force=False):
        if self.active_retry and (
            force or time.monotonic() > self.active_retry[1]
        ):
            path, _deadline, cancelled = self.active_retry
            cancelled.set()
            path.unlink(missing_ok=True)
            self.active_retry = None
            if not force and self.record_cancelled is cancelled:
                self.input_cancelled.set()
                self.error = "Recording retry expired; record again"
                self.phase = "error"
        if self.retry_audio and (
            force or time.monotonic() > self.retry_audio[3]
        ):
            self.retry_audio[0].unlink(missing_ok=True)
            self.retry_audio = None
            if self.retry_lease:
                self.retry_lease.release()
                self.retry_lease = None

    def retry(self):
        with self.lock:
            self._expire_retry()
            if self.recording_active():
                raise RuntimeError("Finish the current recording first")
            if not self.retry_audio:
                raise RuntimeError("No recording available to retry")
            path, token, target, expiry, previous, mode = self.retry_audio
            self.retry_audio = None
            if token != self.token:
                path.unlink(missing_ok=True)
                if self.retry_lease:
                    self.retry_lease.release()
                    self.retry_lease = None
                raise RuntimeError("Voice session changed")
            self.record_cancelled = threading.Event()
            self.record_cancelled.recovery_revision = self.recovery_revision
            self.input_cancelled = threading.Event()
            lease, self.retry_lease = self.retry_lease, None
            if lease and lease.ready.done() and (
                lease.ready.cancelled() or lease.ready.exception() is not None
            ):
                # A failed readiness future cannot recover. A new acquisition
                # asks the manager to retry start/readiness without recapturing.
                lease.release()
                lease = None
            self.record_cancelled.engine_lease = lease
            self.active_retry = (path, expiry, self.record_cancelled)
            self.phase = "transcribing"
            self.error = None
            self.worker = threading.Thread(
                target=self._record,
                args=(path, token, target, self.record_cancelled,
                      previous, mode, True, expiry), daemon=True,
            )
            self.worker.start()

    def _probe_managed(self, target):
        if not target.get('managed'):
            return
        try:
            validated = self._validate_attachment(target)
            if any(
                validated[k] != target[k]
                for k in (
                    'start',
                    'socket_device',
                    'socket_inode',
                    'adapter_device',
                    'adapter_inode',
                )
            ):
                raise RuntimeError('Pi voice identity changed')
            self.terminal.validate_target(target)
        except (RuntimeError, ValueError, OSError, KeyError, TypeError) as exc:
            with self.lock:
                entry = self.sessions.get(target['token'])
                if entry and entry['target'] == target:
                    self._remove_session(target['token'], retire=False)
                    self._save_selection()
            raise RuntimeError(
                'Pi voice disconnected; reconnect before delivery'
            ) from exc

    def _delivery_outcome(self, token, text, operation, cancelled, outcome):
        # Delivery acknowledgement and permission to arm Send are independent.
        # A late acknowledgement must also resolve any target-loss recovery.
        with self.lock:
            operation.delivery_outcome = outcome
            if cancelled is not None:
                cancelled.delivery_outcome = outcome
            retained = self.retained_dictation
            if (
                retained
                and retained.source_token == token
                and retained.text == text
            ):
                if outcome == 'accepted':
                    self.retained_dictation = None
                else:
                    self.recovery_uncertain = True

    def stage(self, text, token, cancelled=None):
        with self.lock:
            self._require_ready(token)
            if not self.target or token != self.token:
                raise RuntimeError(
                    "Voice session changed; discarded the old transcription"
                )
            text = dictation_text(text)
            if not text:
                self.draft = False
                self.pending = None
                self.phase = "idle"
                return False
            if has_control_characters(text):
                raise RuntimeError(
                    "Discarded dictation containing terminal control characters"
                )
            target = self.target
            operation = self.input_cancelled
        if not self.delivery_lock.acquire(blocking=False):
            raise RuntimeError("Input delivery is already in progress")
        try:
            if operation.is_set() or (cancelled and cancelled.is_set()):
                return False
            self._probe_managed(target)
            if operation.is_set() or (cancelled and cancelled.is_set()):
                return False
            guarded = getattr(self.terminal, "insert_guarded", None)
            if guarded:
                guarded(target, text, operation)
            else:
                self.terminal.insert(target, text)
            self._delivery_outcome(token, text, operation,
                                   cancelled, 'accepted')
        except DeliveryUncertain:
            self._delivery_outcome(token, text, operation, cancelled, 'unknown')
            with self.lock:
                # This also covers Send retrying a previously retained draft.
                if token == self.token and operation is self.input_cancelled:
                    self.pending = None
                    self.draft = False
            raise
        except RuntimeError:
            raise
        except Exception as exc:
            self._delivery_outcome(token, text, operation, cancelled, 'unknown')
            raise DeliveryUncertain(
                'Could not confirm delivery; check the prompt') from exc
        finally:
            self.delivery_lock.release()
        with self.lock:
            if (token != self.token or operation.is_set()
                    or (cancelled and cancelled.is_set())):
                return False
            self.draft = True
            self.pending = None
            self.phase = "draft"
            self.error = None
            return True

    def send(self, expected=None, allow_edited=False):
        with self.lock:
            self._require_ready(self.token)
            if expected is not None and expected != (
                self.token, self.input_cancelled
            ):
                raise RuntimeError("Voice session changed; press Send again")
            if expected is not None and expected[1].is_set():
                raise RuntimeError("Voice delivery was cancelled")
            if self.recording_active():
                raise RuntimeError(
                    "Finish recording and transcription before sending"
                )
            pending, token = self.pending, self.token
        if pending:
            self.stage(pending, token)
        with self.lock:
            if expected is not None and (
                expected != (self.token, self.input_cancelled)
                or expected[1].is_set()
            ):
                raise RuntimeError("Voice delivery was cancelled")
            if not self.target or not self.draft:
                raise RuntimeError("No new dictation to send")
            if self.input_cancelled.is_set():
                self.input_cancelled = threading.Event()
            target, operation = self.target, self.input_cancelled
        if not self.delivery_lock.acquire(blocking=False):
            raise RuntimeError("Input delivery is already in progress")
        try:
            self._probe_managed(target)
            self.terminal.validate(target)
            with self.lock:
                if token != self.token or operation.is_set():
                    raise RuntimeError("Voice delivery was cancelled")
                if not self.draft:
                    raise RuntimeError("No new dictation to send")
                # Never retry an ambiguous Enter automatically.
                self.draft = False
                self.phase = "idle"
            guarded = getattr(self.terminal, "submit_guarded", None)
            if guarded:
                if allow_edited:
                    guarded(target, operation, allow_edited=True)
                else:
                    guarded(target, operation)
            else:
                self.terminal.submit(target)
        finally:
            self.delivery_lock.release()

    def stop(self, discard=True, *, target_lost=False):
        if not target_lost:
            self.recovery_revision += 1
            self.record_cancelled.voice_target_lost = False
        self.input_cancelled.set()
        self.cancelled.set()
        self.record_cancelled.set()
        with self.lock:
            self.cancelled.set()
            self.record_cancelled.set()
            if self.capture:
                threading.Thread(
                    target=self.capture.close, daemon=True
                ).start()
            self.capture = None
            self.record_started = None
            if discard:
                self.pending = None
            self._expire_retry(force=True)
            self.phase = "draft" if self.draft or self.pending else "idle"
            self.error = None
            self.audio.stop()


def runtime_dir():
    runtime = (
        Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        / "codex-voice"
    )
    runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
    return runtime


def desktop_notice(title, detail=""):
    print(f"{title}: {detail}", file=sys.stderr, flush=True)
    try:
        subprocess.run(
            [
                "notify-send",
                "--app-name=Agent Voice",
                "--expire-time=4000",
                "--hint=string:x-canonical-private-synchronous:codex-voice",
                title,
                detail,
            ],
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def dispatch(app, request):
    action = request["action"]
    if isinstance(action, str) and action.startswith("select:"):
        app.select(action.split(":", 1)[1])
        return app.status()
    if isinstance(action, str) and action.startswith("voice:"):
        app.audio.set_voice(action.split(":", 1)[1])
        return app.status()
    if action == 'team-toggle':
        with app.lock:
            app.show_team = not app.show_team
            app._save_selection()
        return app.status()
    if action == 'warm':
        with app.lock:
            app._population()
            if app.engines:
                app.engines.warm()
        return app.status()
    if action == 'attach':
        return {'attached': app.attach(request.get('target') or {})}
    if action == 'detach':
        return {'accepted': app.detach(request.get('token'), request)}
    if action in ('recover-copy', 'recover-stage', 'recover-discard'):
        getattr(app, action.replace('-', '_'))()
        return app.status()
    if action == "harness-event":
        return {"accepted": app.harness_event(
            request["token"], request["event"]
        )}
    if action == "register":
        app.register(request["token"], request["target"])
    elif action == "unregister":
        app.unregister(request["token"])
    elif action == "notify":
        return {"accepted": app.notify(request["token"], request["event"])}
    elif action == "auto":
        with app.lock:
            app.auto = bool(request["enabled"])
    elif action == "auto-toggle":
        with app.lock:
            app.auto = not app.auto
    elif action in ("append", "replace"):
        app.record(mode=action)
    elif action == "discard":
        app.stop()
    elif action in (
        "interact", "record", "send", "read", "stop", "retry", "rebind"
    ):
        getattr(app, action)()
    elif action != "status":
        raise RuntimeError("Unknown voice command")
    return app.status()


def serve(runtime, config):
    from voice_tray import VoiceTray

    audio = LocalAudio(runtime, config)
    engines = EngineManager(
        readiness=audio.readiness,
        engines=(
            ('stt', 'tts') if config.get('tts_enabled', True) else ('stt',)
        ),
    )
    audio.engines = engines
    app = Controller(
        runtime, AgentTerminal(), audio, desktop_notice,
        catalogue=SnapshotCache(), engines=engines,
    )
    for stale in runtime.glob("*.wav"):
        stale.unlink(missing_ok=True)
    app.auto = config.get("auto_speak", True)
    app.restore()
    engines.reconcile_startup()
    def tray_action(action):
        try:
            dispatch(app, {"action": action})
        except Exception as exc:
            app.report_error(exc)
            desktop_notice("Codex voice", str(exc))

    tray = VoiceTray(app.status, tray_action)
    path = runtime / "control.sock"
    path.unlink(missing_ok=True)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            try:
                raw = self.rfile.readline(1024 * 1024)
                if not raw.endswith(b"\n"):
                    raise RuntimeError("Voice request exceeds the size limit")
                response = {"ok": True, **dispatch(app, json.loads(raw))}
            except Exception as exc:
                response = {"ok": False, "error": str(exc)}
                app.report_error(exc)
                desktop_notice("Codex voice", str(exc))
            self.wfile.write(json.dumps(response).encode() + b"\n")

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    with Server(str(path), Handler) as server:
        os.chmod(path, 0o600)
        tray.start()

        def shutdown(_sig, _frame):
            with app.lock:
                app.attachments_closed = True
            app.stop()
            threading.Thread(target=server.shutdown, daemon=True).start()

        signal.signal(signal.SIGTERM, shutdown)
        signal.signal(signal.SIGINT, shutdown)
        monitor_stop = threading.Event()

        def monitor():
            while not monitor_stop.wait(2):
                try:
                    app.monitor_attachments()
                except RuntimeError:
                    if not monitor_stop.is_set():
                        raise

        threading.Thread(target=monitor, daemon=True).start()
        try:
            server.serve_forever(poll_interval=0.2)
        finally:
            monitor_stop.set()
            with app.lock:
                app.attachments_closed = True
            tray.stop()
            app.stop()
            try:
                app.engines.close()
            finally:
                app.catalogue.close()
            path.unlink(missing_ok=True)


def call(request, start=True):
    path = runtime_dir() / "control.sock"
    if start:
        subprocess.run(
            ["systemctl", "--user", "start", "codex-voice.service"],
            check=True,
            timeout=15,
        )
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(15)
        # systemd Type=simple may return before the socket is bound.
        for attempt in range(40):
            try:
                sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if not start or attempt == 39:
                    raise RuntimeError("Codex voice service is unavailable")
                time.sleep(0.05)
        sock.sendall(json.dumps(request).encode() + b"\n")
        with sock.makefile("rb") as incoming:
            response = json.loads(incoming.readline(1024 * 1024))
    if not response.get("ok"):
        raise RuntimeError(response.get("error", "Voice command failed"))
    return response


def installed_pi_extension():
    path = Path.home() / '.pi/agent/extensions/pi-voice.js'
    if not path.is_file():
        raise RuntimeError(
            'The Pi voice extension is not installed; run hmswitch first')
    return path


def launcher_command(harness, args, directory, notify):
    if harness not in ("codex", "pi", "qwen-pi"):
        raise RuntimeError("Unsupported voice harness")
    if harness in ("pi", "qwen-pi"):
        if any(arg in ("--print", "-p", "--mode")
               or arg.startswith(("--mode=", "--print=")) for arg in args):
            raise RuntimeError("Voice launchers require an interactive session")
        if harness == 'pi' and any(
            arg in ('--no-extensions', '-ne') for arg in args
        ):
            raise RuntimeError(
                'The Pi voice launcher requires the installed voice extension')
        installed_pi_extension()
        return [harness, *args]
    if args[:1] == ["exec"]:
        raise RuntimeError("Voice launchers require an interactive session")
    return ["codex", "-c", f"notify={notify}", *args]


def launch(args, harness="codex"):
    if os.environ.get("HERDR_ENV") != "1" or not os.environ.get(
        "HERDR_PANE_ID"
    ):
        raise RuntimeError(
            "Run the voice launcher inside the Herdr pane you want to use"
        )
    token = uuid.uuid4().hex
    executable = os.environ.get("CODEX_VOICE_COMMAND", "codex-voice")
    notify = json.dumps([executable, "notify", token])
    directory = runtime_dir() / token
    command = launcher_command(harness, args, directory, notify)
    call({"action": "status"})
    directory.mkdir(mode=0o700)
    env = dict(
        os.environ, AGENT_VOICE_TOKEN=token, AGENT_VOICE_KIND=harness,
        AGENT_VOICE_SOCKET=str(runtime_dir() / "control.sock"),
        AGENT_VOICE_ADAPTER_SOCKET=str(directory / "pi.sock"),
        AGENT_VOICE_LAUNCH_PID=str(os.getpid()),
    )
    child = None
    previous = None
    try:
        call({'action': 'warm'})
        # Existing wrappers retain provider settings, credentials and MCPs.
        child = subprocess.Popen(command, env=env)
        previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
        target = {
            "pane": os.environ["HERDR_PANE_ID"],
            "socket": os.environ["HERDR_SOCKET_PATH"],
            "pid": child.pid,
            "start": process_start(child.pid),
            "harness": harness,
            "adapter_socket": str(directory / "pi.sock"),
        }
        call({"action": "register", "token": token, "target": target})
        return child.wait()
    finally:
        if previous is not None:
            signal.signal(signal.SIGINT, previous)
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        try:
            call({"action": "unregister", "token": token}, start=False)
        except (OSError, RuntimeError):
            pass
        (directory / "pi.sock").unlink(missing_ok=True)
        try:
            directory.rmdir()
        except OSError:
            pass


def main(args=None):
    args = list(sys.argv[1:] if args is None else args)
    try:
        if args and args[0] == "serve":
            config_path = os.environ.get(
                "CODEX_VOICE_CONFIG",
                str(Path.home() / ".config/codex-voice/config.json"),
            )
            serve(runtime_dir(), json.loads(Path(config_path).read_text()))
            return 0
        if args and args[0] == "notify":
            if len(args) != 3:
                return 2
            # Callbacks must not start services or hold up a completed turn.
            event = json.loads(args[2])
            if is_cli_thread(event.get("thread-id")):
                call(
                    {"action": "notify", "token": args[1], "event": event},
                    start=False,
                )
            return 0
        if args and args[0] in (
            "interact",
            "record",
            "send",
            "read",
            "stop",
            "status",
            "auto",
            "voice",
            "retry",
            "rebind",
            "discard",
            "append",
            "replace",
            "recover-copy",
            "recover-stage",
            "recover-discard",
        ):
            request = {"action": args[0]}
            if args[0] == "auto":
                if len(args) != 2 or args[1] not in ("on", "off"):
                    raise RuntimeError("Usage: codex-voice auto on|off")
                request["enabled"] = args[1] == "on"
            if args[0] == "voice":
                if len(args) != 2:
                    raise RuntimeError("Usage: codex-voice voice CHARACTER")
                request["action"] = "voice:" + args[1]
            response = call(request)
            if args[0] in ("status", "auto", "voice"):
                response.pop("reply", None)
                print(json.dumps(response, indent=2))
            return 0
        if args and args[0] in ("--help", "-h"):
            print(
                "Usage: <codex|pi|qwen-pi>-voice [options]\n"
                "       codex-voice interact|record|send|read|stop|status\n"
                "       codex-voice retry|rebind|discard|append|replace\n"
                "       codex-voice recover-copy|recover-stage"
                "|recover-discard\n"
                "       codex-voice auto on|off\n"
                "       codex-voice voice CHARACTER\n\n"
                "Super+Space: record/stop/send draft. "
                "Super+Shift+Space: send. "
                "Super+R: read/stop.\n"
                "Run inside Herdr. Prefix Codex options with -- "
                "if they conflict.\n"
                "Stop background engines: systemctl --user stop "
                "codex-voice{,-stt,-tts}.service"
            )
            return 0
        return launch(
            args[1:] if args[:1] == ["--"] else args,
            os.environ.get("AGENT_VOICE_LAUNCH_KIND", "codex"),
        )
    except Exception as exc:
        print(f"codex-voice: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
