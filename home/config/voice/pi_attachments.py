"""Managed Pi identity policy. Call registry methods under one lock."""

from dataclasses import dataclass
import os
from pathlib import Path
import stat
import uuid

HEARTBEAT_SECONDS = 5
LEASE_SECONDS = 15
# Reserve room for JSON escaping (up to six bytes per character) and identity.
MAX_RETAINED_BYTES = (256 * 1024 - 8192) // 6


@dataclass(frozen=True)
class PaneKey:
    socket_path: str
    socket_device: int
    socket_inode: int
    pane_id: str


@dataclass(frozen=True)
class BridgeIdentity:
    pane: PaneKey
    pid: int
    process_start: str
    bridge_id: str


@dataclass(frozen=True)
class Admission:
    state: str
    token: str | None = None
    replaced: str | None = None


@dataclass(frozen=True)
class RetainedDictation:
    text: str
    source_token: str
    source_session: str
    source_label: str


def can_auto_select(
    *, selection_initialized, selected_token, ready, team_child
):
    return (
        not selection_initialized
        and selected_token is None
        and ready
        and not team_child
    )


def pane_key(target):
    return PaneKey(
        target['socket'],
        target['socket_device'],
        target['socket_inode'],
        target['pane'],
    )


def bridge_identity(target):
    return BridgeIdentity(
        pane_key(target), target['pid'], target['start'], target['bridge_id']
    )


def socket_instance(path):
    canonical = Path(path).resolve(strict=True)
    info = canonical.stat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError('Herdr endpoint is not a same-user Unix socket')
    return str(canonical), info.st_dev, info.st_ino


def validate_endpoint(runtime, path, pid, bridge_id):
    try:
        if str(uuid.UUID(bridge_id)) != bridge_id:
            raise ValueError('Noncanonical bridge UUID')
        runtime, endpoint = Path(runtime), Path(path)
        parent = runtime.lstat()
        if (
            not stat.S_ISDIR(parent.st_mode)
            or parent.st_uid != os.getuid()
            or stat.S_IMODE(parent.st_mode) & 0o077
            or runtime.resolve() != runtime.absolute()
        ):
            raise ValueError('Pi runtime directory must be private and owned')
        if (
            not endpoint.is_absolute()
            or endpoint.parent != runtime.absolute()
            or endpoint.name != f'pi-{pid}-{bridge_id}.sock'
        ):
            raise ValueError(
                'Pi endpoint must use its private bridge UUID path'
            )
        info = endpoint.lstat()
        if (
            not stat.S_ISSOCK(info.st_mode)
            or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) & 0o077
        ):
            raise ValueError(
                'Pi endpoint must be a private same-user Unix socket'
            )
        return info.st_dev, info.st_ino
    except (OSError, TypeError, AttributeError) as exc:
        raise ValueError('Pi endpoint is unavailable or invalid') from exc


def event_matches(target, event, *, session=True):
    fields = ['bridge_id', 'activation', 'pid']
    if session:
        fields += ['session', 'harness']
    return all(event.get(key) == target.get(key) for key in fields)


class AttachmentRegistry:
    def __init__(self):
        self.identities = {}
        self.tokens = {}
        self.panes = {}
        self.revisions = {}
        self.highwater = {}
        self.retired = set()

    @staticmethod
    def slot(pane):
        # Fence even a changed server instance at the same textual address.
        return pane.socket_path, pane.pane_id

    def revision(self, pane):
        return self.revisions.get(self.slot(pane), 0)

    def lookup(self, identity):
        return self.identities.get(identity)

    def admit(self, identity, activation, revision, token=None):
        process = (identity.pid, identity.process_start)
        generation = (*process, identity.bridge_id)
        highest, bridge = self.highwater.get(process, (0, None))
        if generation in self.retired or activation < highest:
            return Admission('superseded')
        if activation == highest and bridge != identity.bridge_id:
            raise ValueError('Pi activation ordinal is already owned')
        if self.revision(identity.pane) != revision:
            return Admission('retry')
        existing = self.lookup(identity)
        if existing:
            return Admission('existing', existing)
        slot = self.slot(identity.pane)
        old = self.panes.get(slot)
        if old:
            self.remove(old)
        token = token or str(uuid.uuid4())
        self.highwater[process] = (activation, identity.bridge_id)
        self.identities[identity] = token
        self.tokens[token] = identity
        self.panes[slot] = token
        self.revisions[slot] = self.revisions.get(slot, 0) + 1
        return Admission('admitted', token, old)

    def remove(self, token, *, retire=True):
        identity = self.tokens.pop(token, None)
        if identity is None:
            return
        if retire:
            self.retired.add(
                (identity.pid, identity.process_start, identity.bridge_id)
            )
        self.identities.pop(identity, None)
        slot = self.slot(identity.pane)
        if self.panes.get(slot) == token:
            self.panes.pop(slot)
        self.revisions[slot] = self.revisions.get(slot, 0) + 1

    def fences(self):
        return {
            'highwater': [
                [pid, start, ordinal, bridge]
                for (pid, start), (ordinal, bridge) in self.highwater.items()
            ],
            'retired': [list(item) for item in self.retired],
        }

    def restore_fences(self, saved):
        self.highwater = {
            (pid, start): (ordinal, bridge)
            for pid, start, ordinal, bridge in saved.get('highwater', [])
        }
        self.retired = {tuple(item) for item in saved.get('retired', [])}

    def prune(self, starts, captured=None):
        """Release fences only after their process incarnation disappears."""
        captured = (
            set(self.highwater) | {item[:2] for item in self.retired}
            if captured is None else set(captured)
        )
        self.highwater = {
            key: value
            for key, value in self.highwater.items()
            if (key not in captured or key[0] not in starts
                or starts[key[0]] == key[1])
        }
        self.retired = {
            item
            for item in self.retired
            if (item[:2] not in captured or item[0] not in starts
                or starts[item[0]] == item[1])
        }
