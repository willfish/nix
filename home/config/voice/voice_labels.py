"""Immutable Herdr presentation snapshots and pure Pi labels.

Call set_active/refresh from background maintenance, never from status
rendering.
The injected runner(key, timeout) returns the Herdr snapshot object and must
obey
its timeout. Injected schedulers implement nonblocking submit(callable).
"""

from collections import Counter
from collections.abc import Mapping
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field, replace
import hashlib
import json
import os
import subprocess
import threading
import time
from types import MappingProxyType
from typing import Literal
import unicodedata


@dataclass(frozen=True, order=True)
class SocketKey:
    socket_path: str
    socket_device: int
    socket_inode: int


@dataclass(frozen=True)
class SnapshotState:
    panes: Mapping[str, Mapping[str, str]] = field(default_factory=dict)
    fetched_at: float = 0.0
    attempted_at: float = 0.0
    outcome: Literal['unknown', 'ok', 'unavailable'] = 'unknown'
    inflight: bool = False

    def __post_init__(self):
        object.__setattr__(
            self,
            'panes',
            MappingProxyType(
                {
                    key: MappingProxyType(dict(value))
                    for key, value in self.panes.items()
                }
            ),
        )

    def authoritative(self, now):
        """Fresh catalogue evidence only; process/socket admission checks
        remain required.
        """
        return self.outcome == 'ok' and 0 <= now - self.fetched_at < 3


def parse_snapshot(data):
    """Flatten the Herdr session.snapshot schema; malformed is not empty
    success.
    """
    if not isinstance(data, dict):
        raise ValueError('snapshot must be an object')
    indexes = []
    for name, identity in [('workspaces', 'workspace_id'), ('tabs', 'tab_id'),
                           ('panes', 'pane_id')]:
        rows = data.get(name)
        if not isinstance(rows, list):
            raise ValueError(f'missing snapshot {name}')
        index = {}
        for row in rows:
            if not isinstance(row, dict) or not isinstance(
                row.get(identity), str
            ):
                raise ValueError(f'invalid snapshot {name}')
            if row[identity] in index:
                raise ValueError(f'duplicate snapshot {identity}')
            index[row[identity]] = row
        indexes.append(index)
    workspaces, tabs, panes = indexes
    return {
        identity: {
            'workspace': clean(
                workspaces.get(pane.get('workspace_id'), {}).get('label')
            ),
            'tab': clean(tabs.get(pane.get('tab_id'), {}).get('label')),
            'display_agent': clean(pane.get('display_agent')),
        }
        for identity, pane in panes.items()
    }


def run_snapshot(key, timeout):
    def identity():
        info = os.stat(key.socket_path)
        return info.st_dev, info.st_ino
    expected = key.socket_device, key.socket_inode
    if identity() != expected:
        raise OSError('Herdr socket instance changed')
    result = subprocess.run(
        ['herdr', 'api', 'snapshot'],
        env={**os.environ, 'HERDR_SOCKET_PATH': key.socket_path},
        capture_output=True, text=True, check=True, timeout=timeout)
    if identity() != expected:
        raise OSError('Herdr socket instance changed')
    response = json.loads(result.stdout)
    return response['result']['snapshot']


class SnapshotCache:
    def __init__(self, *, runner=run_snapshot, clock=time.monotonic,
                 scheduler=None, max_keys=64):
        if max_keys < 1:
            raise ValueError('max_keys must be positive')
        self.runner, self.clock, self.max_keys = runner, clock, max_keys
        self._owned = scheduler is None
        self._scheduler = scheduler or ThreadPoolExecutor(
            max_workers=min(8, max_keys))
        self._lock = threading.Lock()
        self._entries = {}
        self._tickets = {}
        self._pending = 0
        self._closed = False

    def set_active(self, keys):
        keys = set(keys)
        if len(keys) > self.max_keys:
            raise ValueError('too many active Herdr instances')
        with self._lock:
            if self._closed:
                raise RuntimeError('snapshot cache closed')
            self._entries = {key: self._entries.get(
                key, SnapshotState()) for key in keys}
            self._tickets = {key: self._tickets.get(
                key, object()) for key in keys}

    def read(self, key):
        with self._lock:
            return self._entries.get(key, SnapshotState())

    def snapshots(self):
        with self._lock:
            return MappingProxyType(dict(self._entries))

    def refresh(self, key):
        with self._lock:
            state = self._entries.get(key)
            if self._closed or state is None or state.inflight:
                return False
            if (
                state.outcome != 'unknown'
                and self.clock() - state.attempted_at < 3
            ):
                return False
            # Retired in-flight tasks also count, bounding churn as well as
            # active keys.
            if self._pending >= self.max_keys:
                return False
            ticket = self._tickets[key]
            self._pending += 1
            self._entries[key] = replace(
                state, attempted_at=self.clock(), inflight=True)
        try:
            self._scheduler.submit(lambda: self._refresh(key, ticket))
        except Exception:
            self._publish(key, ticket, None)
        return True

    def _refresh(self, key, ticket):
        try:
            panes = parse_snapshot(self.runner(key, 1))
        except Exception:
            panes = None
        self._publish(key, ticket, panes)

    def _publish(self, key, ticket, panes):
        with self._lock:
            self._pending -= 1
            if self._closed or self._tickets.get(key) is not ticket:
                return
            previous = self._entries[key]
            now = self.clock()
            self._entries[key] = SnapshotState(
                panes=previous.panes if panes is None else panes,
                fetched_at=previous.fetched_at if panes is None else now,
                attempted_at=now,
                outcome='unavailable' if panes is None else 'ok',
            )

    def close(self):
        with self._lock:
            self._closed = True
            self._entries.clear()
            self._tickets.clear()
        if self._owned:
            self._scheduler.shutdown(wait=True, cancel_futures=True)


def clean(value):
    if not isinstance(value, str):
        return ''
    # Drop terminal controls, bidi overrides and zero-width formatting.
    # Keep marks.
    return ' '.join(
        ''.join(
            ' ' if unicodedata.category(c).startswith('C') else c for c in value
        ).split()
    )


def display_width(text):
    """Conservative stdlib width: ambiguous=1, W/F=2, combining marks=0.

    Emoji sequences are counted per codepoint (often wider than actual
    rendering).
    Terminal-specific ambiguous-width modes may differ; no grapheme dependency.
    """
    return sum(
        (
            0
            if unicodedata.category(c) in ('Mn', 'Me')
            else 2 if unicodedata.east_asian_width(c) in ('W', 'F') else 1
        )
        for c in text
    )


def _truncate(text, budget):
    if display_width(text) <= budget:
        return text
    result = ''
    for c in text:
        if display_width(result + c) > budget - 1:
            break
        result += c
    return result.rstrip() + '…' if budget else ''


def model_thinking(row, metadata):
    model = row.get('model')
    if isinstance(model, dict):
        model = model.get('id') or model.get('name')
    model, thinking = clean(model), clean(row.get('thinking'))
    parts = [
        part.strip() for part in metadata.get('display_agent', '').split('·')
    ]
    if len(parts) == 3 and parts[0] in ('pi', 'qwen-pi'):
        thinking = thinking or parts[1]
        model = model or parts[2]
    return model, thinking


def build_labels(entries, snapshots):
    """Return copied rows with Pi label/full_label, leaving legacy rows
    unchanged.

    Each Pi row supplies harness, pane, socket_key (SocketKey), optional model,
    thinking, team_child, selected. token/id are preserved, never used as
    labels.
    snapshots maps SocketKey to SnapshotState. Pass exactly the visible rows.
    """
    rows = [dict(row) for row in entries]
    records = []
    for index, row in enumerate(rows):
        if row.get('harness') not in ('pi', 'qwen-pi'):
            continue
        key, pane = row['socket_key'], clean(row['pane'])
        state = snapshots.get(key, SnapshotState())
        metadata = {name: clean(value) for name, value in
                    state.panes.get(row['pane'], {}).items()}
        if not pane:
            raise ValueError('Pi label requires a full pane identity')
        records.append((index, key, pane, metadata))
    short_counts = Counter(pane.rsplit(':', 1)[-1] for _, _, pane, _ in records)
    full_counts = Counter(pane for _, _, pane, _ in records)
    # Stable digest lengths expand on a prefix collision, independent of row
    # order.
    identities = {(key, pane) for _, key, pane, _ in records}
    hashes = {
        identity: hashlib.sha256(
            json.dumps(
                [
                    identity[0].socket_path,
                    identity[0].socket_device,
                    identity[0].socket_inode,
                    identity[1],
                ],
                ensure_ascii=True,
            ).encode()
        ).hexdigest()
        for identity in identities
    }
    digest_size = 6
    while len({h[:digest_size] for h in hashes.values()}) < len(hashes):
        digest_size += 1
    prepared = {}
    for index, key, pane, metadata in records:
        row = rows[index]
        model, thinking = model_thinking(row, metadata)
        workspace, tab = metadata.get('workspace', ''), metadata.get('tab', '')
        pieces = ['pi']
        if workspace:
            pieces.append(workspace)
        if tab and tab != workspace:
            pieces.append(tab)
        pieces.extend(part for part in (thinking, model) if part)
        flags = []
        if row.get('selected'):
            flags.append('selected')
        if row.get('team_child'):
            flags.append('team')
        prepared[index] = (' · '.join(pieces), flags,
                           bool(workspace or tab), key, pane)

    def render(index, with_identity):
        body, flags, named, key, pane = prepared[index]
        suffix = list(flags)
        if with_identity:
            identity = pane if not named or short_counts[pane.rsplit(
                ':', 1)[-1]] > 1 else pane.rsplit(':', 1)[-1]
            if full_counts[pane] > 1:
                identity += '@' + hashes[key, pane][:digest_size]
            suffix.append(identity)
        tail = (' · ' + ' · '.join(suffix)) if suffix else ''
        rows[index]['full_label'] = body + tail
        if display_width(tail) > 50:
            # Keep the discriminator even when an identity cannot fit in full.
            tail = ' · ' + ' · '.join(
                [*flags, '@' + hashes[key, pane][:digest_size]])
        rows[index]['label'] = _truncate(
            body, max(0, 55 - display_width(tail))) + tail

    # Selection is transient, so identical names need stable identities even
    # when only one row currently carries the selected marker.
    names = Counter(item[0] for item in prepared.values())
    identified = {index for index, item in prepared.items()
                  if not item[2] or names[item[0]] > 1}
    while True:
        for index in prepared:
            render(index, index in identified)
        counts = Counter(rows[index]['label'] for index in prepared)
        collisions = {index for index in prepared
                      if counts[rows[index]['label']] > 1} - identified
        if not collisions:
            return rows
        # Adding a suffix shortens the available name width. Check again so
        # truncation cannot introduce a new ambiguity with another row.
        identified.update(collisions)
