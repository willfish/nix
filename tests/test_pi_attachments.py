"""Pure managed Pi admission and private endpoint contracts."""

import sys
from pathlib import Path
import os
import socket
import tempfile
import unittest
import uuid
from dataclasses import replace

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / 'home/config/voice')
)
from pi_attachments import (
    PaneKey,
    BridgeIdentity,
    AttachmentRegistry,
    can_auto_select,
    validate_endpoint,
)


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.pane = PaneKey('/tmp/herdr.sock', 1, 2, 'w1:p1')
        self.identity = BridgeIdentity(self.pane, 42, '100', str(uuid.uuid4()))
        self.registry = AttachmentRegistry()

    def test_distinct_incarnations(self):
        for other in (
            replace(self.identity, process_start='101'),
            replace(self.identity, bridge_id=str(uuid.uuid4())),
            replace(self.identity, pane=replace(self.pane, socket_inode=3)),
        ):
            self.assertNotEqual(self.identity, other)

    def test_first_ready_ordinary_only(self):
        for initialized, selected, ready, child, expected in (
            (False, None, False, False, False),
            (False, None, True, True, False),
            (False, None, True, False, True),
            (True, None, True, False, False),
            (False, 'legacy', True, False, False),
        ):
            self.assertEqual(
                can_auto_select(
                    selection_initialized=initialized,
                    selected_token=selected,
                    ready=ready,
                    team_child=child,
                ),
                expected,
            )

    def test_idempotent_token_and_revision(self):
        result = self.registry.admit(self.identity, 1, 0)
        self.assertEqual(result.state, 'admitted')
        uuid.UUID(result.token)
        revision = self.registry.revision(self.pane)
        retry = self.registry.admit(self.identity, 1, revision)
        self.assertEqual(retry.token, result.token)
        self.assertEqual(retry.state, 'existing')
        self.assertEqual(self.registry.revision(self.pane), revision)

    def test_reversed_reload_rejects_never_admitted_older_activation(self):
        newer = replace(self.identity, bridge_id=str(uuid.uuid4()))
        winner = self.registry.admit(newer, 2, 0)
        late = self.registry.admit(self.identity, 1, 0)
        self.assertEqual(late.state, 'superseded')
        self.assertIsNone(late.token)
        self.assertEqual(self.registry.lookup(newer), winner.token)

    def test_revision_requires_different_process_to_revalidate(self):
        self.registry.admit(self.identity, 1, 0)
        other = replace(self.identity, pid=43)
        self.assertEqual(self.registry.admit(other, 1, 0).state, 'retry')
        result = self.registry.admit(
            other, 1, self.registry.revision(self.pane)
        )
        self.assertEqual(result.state, 'admitted')
        self.assertEqual(
            self.registry.admit(
                self.identity, 1, self.registry.revision(self.pane)
            ).state,
            'superseded',
        )

    def test_pruning_an_old_probe_does_not_forget_newly_admitted_process(self):
        self.registry.admit(self.identity, 2, 0)
        self.registry.prune({99: None})
        late = replace(self.identity, bridge_id=str(uuid.uuid4()))
        self.assertEqual(
            self.registry.admit(
                late, 1, self.registry.revision(self.pane)
            ).state,
            'superseded',
        )

    def test_ordinal_collision_is_invalid(self):
        self.registry.admit(self.identity, 1, 0)
        other = replace(self.identity, bridge_id=str(uuid.uuid4()))
        with self.assertRaises(ValueError):
            self.registry.admit(other, 1, self.registry.revision(self.pane))

    def test_expiry_allows_same_bridge_reattach_but_detach_retires(self):
        result = self.registry.admit(self.identity, 1, 0)
        self.registry.remove(result.token, retire=False)
        again = self.registry.admit(
            self.identity, 1, self.registry.revision(self.pane)
        )
        self.assertEqual(again.state, 'admitted')
        self.registry.remove(again.token)
        self.assertEqual(
            self.registry.admit(
                self.identity, 1, self.registry.revision(self.pane)
            ).state,
            'superseded',
        )


class EndpointTests(unittest.TestCase):
    def test_real_socket_and_rejections_without_unlink(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            bridge = str(uuid.uuid4())
            path = parent / f'pi-{os.getpid()}-{bridge}.sock'
            with socket.socket(socket.AF_UNIX) as endpoint:
                endpoint.bind(str(path))
                path.chmod(0o600)
                endpoint.listen()
                observed = validate_endpoint(
                    parent, str(path), os.getpid(), bridge
                )
                self.assertEqual(
                    observed, (path.stat().st_dev, path.stat().st_ino)
                )
                path.chmod(0o666)
                with self.assertRaises(ValueError):
                    validate_endpoint(parent, str(path), os.getpid(), bridge)
                path.chmod(0o600)
                parent.chmod(0o755)
                with self.assertRaises(ValueError):
                    validate_endpoint(parent, str(path), os.getpid(), bridge)
                parent.chmod(0o700)
                self.assertTrue(path.exists())
            path.unlink()
            path.write_text('unrelated')
            with self.assertRaises(ValueError):
                validate_endpoint(parent, str(path), os.getpid(), bridge)
            path.unlink()
            other = parent / 'control.sock'
            other.write_text('unrelated')
            path.symlink_to(other)
            with self.assertRaises(ValueError):
                validate_endpoint(parent, str(path), os.getpid(), bridge)
            self.assertEqual(other.read_text(), 'unrelated')

    def test_uuid_filename_and_ownership_are_validated(self):
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            bridge = str(uuid.uuid4())
            path = runtime / f'pi-{os.getpid()}-{bridge}.sock'
            with socket.socket(socket.AF_UNIX) as endpoint:
                endpoint.bind(str(path))
                path.chmod(0o600)
                with self.assertRaises(ValueError):
                    validate_endpoint(
                        runtime, str(path), os.getpid(), 'not-a-uuid'
                    )
                with self.assertRaises(ValueError):
                    validate_endpoint(
                        runtime, str(path), os.getpid() + 1, bridge
                    )
                with patch(
                    'pi_attachments.os.getuid', return_value=os.getuid() + 1
                ):
                    with self.assertRaises(ValueError):
                        validate_endpoint(
                            runtime, str(path), os.getpid(), bridge
                        )
