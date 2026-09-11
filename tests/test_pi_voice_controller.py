"""Managed attachment lifecycle without real Pi, Herdr or audio services."""

import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
import uuid
from unittest.mock import patch

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / 'home/config/voice')
)
import voice_controller as voice
from test_voice_controller import Terminal, Audio
from pi_attachments import MAX_RETAINED_BYTES


class ManagedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.runtime = Path(self.tmp.name)
        self.listeners = {}
        self.herdr = socket.socket(socket.AF_UNIX)
        self.addCleanup(self.herdr.close)
        self.herdr.bind(str(self.runtime / 'herdr.sock'))
        self.terminal, self.audio = Terminal(), Audio()
        self.app = voice.Controller(
            self.runtime, self.terminal, self.audio, lambda *a: None
        )
        self.addCleanup(self.app.stop)
        self.lookup = patch.object(voice.Herdr, 'request', self.lookup_pane)
        self.lookup.start()
        self.addCleanup(self.lookup.stop)

    def lookup_pane(self, target, *args):
        self.assertFalse(self.app.lock._is_owned())
        return {
            'process_info': {
                'pane_id': target['pane'],
                'foreground_processes': [{'pid': target['pid'], 'name': 'pi'}],
            }
        }

    def target(self, **changes):
        bridge = str(uuid.uuid4())
        pid = changes.get('pid', os.getpid())
        target = dict(
            pane='w1:p1',
            socket=str(self.runtime / 'herdr.sock'),
            pid=pid,
            harness='pi',
            session='conversation-1',
            bridge_id=bridge,
            activation=1,
            team_child=False,
            model='test',
            thinking='medium',
            adapter_socket=str(self.runtime / f'pi-{pid}-{bridge}.sock'),
        )
        target.update(changes)
        endpoint = socket.socket(socket.AF_UNIX)
        endpoint.bind(target['adapter_socket'])
        Path(target['adapter_socket']).chmod(0o600)
        endpoint.listen()
        self.listeners[target['adapter_socket']] = endpoint
        self.addCleanup(endpoint.close)
        return target

    def attach(self, target):
        return self.app.attach(target)['token']

    def event(self, token, target, kind='ready', **changes):
        event = {
            key: target[key]
            for key in ('bridge_id', 'activation', 'pid', 'session', 'harness')
        }
        return self.app.harness_event(token, dict(event, type=kind, **changes))

    def engine_manager(self):
        from concurrent.futures import Future
        from unittest.mock import Mock
        manager = Mock()
        leases = []
        def acquire(engine):
            lease = Mock(engine=engine)
            lease.ready = Future()
            lease.ready.set_result(None)
            def wait(timeout=None):
                self.assertFalse(self.app.lock._is_owned())
                return lease.ready.result(timeout)
            lease.wait.side_effect = wait
            leases.append(lease)
            return lease
        manager.acquire.side_effect = acquire
        manager.set_population.side_effect = lambda *args: self.assertTrue(
            self.app.lock._is_owned())
        self.app.engines = manager
        return manager, leases

    def test_warm_action_preserves_selection_and_uses_temporary_leases(self):
        manager, _ = self.engine_manager()
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        voice.dispatch(self.app, {'action': 'warm'})
        manager.warm.assert_called_once_with()
        self.assertEqual(self.app.token, token)
        self.assertEqual(self.app.target['session'], target['session'])

    def test_engine_population_tracks_admission_and_removal(self):
        manager, _ = self.engine_manager()
        target = self.target()
        token = self.attach(target)
        self.app.unregister(token)
        counts = [call.args for call in manager.set_population.call_args_list]
        self.assertEqual(counts, [(0, 1), (1, 1), (1, 0), (0, 0)])

    def test_engine_lease_survives_retry_and_record_label_is_pinned(self):
        manager, leases = self.engine_manager()
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        with patch.object(
            self.audio, 'transcribe', side_effect=RuntimeError('offline')
        ):
            self.app.record()
            for _ in range(100):
                if self.app.phase == 'recording':
                    break
                threading.Event().wait(.01)
            self.assertEqual(self.app.phase, 'recording')
            label = self.app.status()['recording_label']
            self.event(token, target, 'metadata', model='replacement')
            self.assertEqual(self.app.status()['recording_label'], label)
            self.assertIn('test', label)
            self.assertEqual(manager.acquire.call_args.args, ('stt',))
            self.app.record()
            self.app.worker.join(2)
        self.assertIsNotNone(self.app.retry_audio)
        leases[0].release.assert_not_called()
        self.app.retry()
        self.app.worker.join(2)
        self.assertFalse(self.app.worker.is_alive())
        self.assertEqual(manager.acquire.call_count, 1)
        leases[0].release.assert_called_once()
        self.assertTrue(self.app.draft)

    def test_retry_reacquires_after_failed_engine_readiness(self):
        from concurrent.futures import Future
        manager, leases = self.engine_manager()
        acquire = manager.acquire.side_effect
        def fail_first(engine):
            lease = acquire(engine)
            if len(leases) == 1:
                lease.ready = Future()
                lease.ready.set_exception(RuntimeError('model not ready'))
            return lease
        manager.acquire.side_effect = fail_first
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.record()
        for _ in range(100):
            if self.app.phase == 'recording':
                break
            threading.Event().wait(.01)
        self.assertEqual(self.app.phase, 'recording')
        self.app.record()
        self.app.worker.join(2)
        self.assertIsNotNone(self.app.retry_audio)
        self.assertEqual(self.audio.transcriptions, 0)
        self.app.retry()
        self.app.worker.join(2)
        self.assertEqual(manager.acquire.call_count, 2)
        for lease in leases:
            lease.release.assert_called_once()
        self.assertEqual(self.audio.transcriptions, 1)
        self.assertTrue(self.app.draft)

    def test_readiness_wait_is_cancellable_without_transcription(self):
        from concurrent.futures import Future
        manager, leases = self.engine_manager()
        acquire = manager.acquire.side_effect
        def pending(engine):
            lease = acquire(engine)
            lease.ready = Future()
            return lease
        manager.acquire.side_effect = pending
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.record()
        for _ in range(100):
            if self.app.phase == 'recording':
                break
            threading.Event().wait(.01)
        self.assertEqual(self.app.phase, 'recording')
        self.app.record()
        for _ in range(100):
            if leases[0].wait.called:
                break
            threading.Event().wait(.01)
        self.assertTrue(leases[0].wait.called)
        self.assertEqual(self.audio.transcriptions, 0)
        self.app.stop()
        self.app.worker.join(1)
        self.assertFalse(self.app.worker.is_alive())
        leases[0].release.assert_called_once()
        self.assertIsNone(self.app.retry_audio)

    def test_speech_releases_engine_after_playback_error(self):
        manager, leases = self.engine_manager()
        def speak(*args):
            self.assertFalse(self.app.lock._is_owned())
            leases[0].release.assert_not_called()
            raise RuntimeError('playback failed')
        with patch.object(self.audio, 'speak', side_effect=speak):
            self.app._speak('hello', threading.Event())
        manager.acquire.assert_called_once_with('tts')
        leases[0].release.assert_called_once()

    def test_selected_root_and_child_speech_and_dictation(self):
        for child in (False, True):
            for auto in (False, True):
                with self.subTest(child=child, auto=auto):
                    target = self.target(
                        team_child=child, activation=1 + 2 * child + auto)
                    token = self.attach(target)
                    self.event(token, target)
                    self.app.select(token)
                    self.app.auto = auto
                    manager, _ = self.engine_manager()
                    with patch.object(voice.threading, 'Thread') as thread, \
                            patch.object(self.audio, 'speak') as speak:
                        # A late reply from an older extension is accepted,
                        # but selection must not make a child audible.
                        self.assertTrue(self.event(
                            token, target, 'reply', turn='late-turn',
                            text='## Summary\nCompleted.'))
                        if child or not auto:
                            thread.assert_not_called()
                        else:
                            thread.return_value.start.assert_called_once()
                        thread.reset_mock()
                        self.app.playback = None
                        if child:
                            with self.assertRaisesRegex(
                                RuntimeError, 'Team members are silent'
                            ):
                                self.app.read()
                            thread.assert_not_called()
                        else:
                            self.app.read()
                            thread.return_value.start.assert_called_once()
                        manager.acquire.assert_not_called()
                        speak.assert_not_called()
                    self.assertEqual(self.app.status()['can_speak'], not child)
                    self.assertEqual(self.app.auto, auto)
                    # Mock playback cannot be allowed to block staging.
                    self.app.playback = None
                    self.app.select(token)
                    self.assertTrue(self.app.stage('Dictated text', token))
                    self.app.stop()
                    self.app.unregister(token)

    def test_managed_reply_acceptance_is_not_notification_selection(self):
        target = self.target(team_child=True)
        token = self.attach(target)
        self.event(token, target)
        for _ in range(2):
            self.assertTrue(self.event(token, target, 'reply',
                            turn='turn', text='Summary'))
        self.assertIsNone(self.app.token)
        self.assertFalse(self.event('unknown', target, 'reply', turn='turn'))

    def test_late_recovery_ack_consumes_text_without_arming_send(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.select(token)
        self.app.retain_dictation('late text', token, target)
        def insert(*args):
            self.app.stop(target_lost=True)
        with patch.object(self.terminal, 'insert', side_effect=insert):
            self.assertFalse(self.app.recover_stage())
        self.assertIsNone(self.app.retained_dictation)
        self.assertFalse(self.app.draft)

    def test_pending_late_ack_updates_recovery_after_target_loss(self):
        for uncertain in (False, True):
            with self.subTest(uncertain=uncertain):
                target = self.target(activation=2 if uncertain else 1)
                token = self.attach(target)
                self.event(token, target)
                self.app.select(token)
                self.app.pending = 'pending text'
                def insert(*args):
                    self.app.unregister(token)
                    if uncertain:
                        raise voice.DeliveryUncertain('lost ack')
                with patch.object(self.terminal, 'insert', side_effect=insert):
                    if uncertain:
                        with self.assertRaises(voice.DeliveryUncertain):
                            self.app.stage('pending text', token)
                    else:
                        self.assertFalse(self.app.stage('pending text', token))
                if uncertain:
                    self.assertTrue(self.app.recovery_uncertain)
                    self.assertEqual(
                        self.app.retained_dictation.text, 'pending text')
                    self.app.recover_discard()
                else:
                    self.assertIsNone(self.app.retained_dictation)

    def test_unexpected_delivery_error_prevents_recovery_restage(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.select(token)
        self.app.retain_dictation('text', token, target)
        with patch.object(
            self.terminal, 'insert', side_effect=ValueError('invalid ack')
        ) as insert:
            with self.assertRaises(voice.DeliveryUncertain):
                self.app.recover_stage()
            with self.assertRaises(RuntimeError):
                self.app.recover_stage()
            self.assertEqual(insert.call_count, 1)

    def test_monitor_prunes_only_captured_process_incarnations(self):
        self.app.attachments.highwater[(42, 'old')] = (1, 'old-bridge')
        def process(pid):
            with self.app.lock:
                self.app.attachments.highwater[(42, 'new')] = (2, 'new-bridge')
                self.app.attachments.retired.add((42, 'new', 'new-bridge'))
            return None
        with patch.object(voice, 'process_start', side_effect=process):
            self.app.monitor_attachments()
        self.assertNotIn((42, 'old'), self.app.attachments.highwater)
        self.assertIn((42, 'new'), self.app.attachments.highwater)
        self.assertIn((42, 'new', 'new-bridge'), self.app.attachments.retired)

    def test_labels_are_cached_off_lock_and_team_toggle_persists(self):
        from voice_labels import SnapshotCache
        from unittest.mock import Mock
        queued = []
        scheduler = Mock()
        scheduler.submit.side_effect = queued.append
        def snapshot(key, timeout):
            self.assertFalse(self.app.lock._is_owned())
            return {
                'workspaces': [{'workspace_id': 'w', 'label': 'Workspace'}],
                'tabs': [],
                'panes': [{'pane_id': 'w1:p1', 'workspace_id': 'w'}],
            }
        self.app.catalogue = SnapshotCache(runner=snapshot, scheduler=scheduler)
        self.addCleanup(self.app.catalogue.close)
        target = self.target(team_child=True)
        token = self.attach(target)
        self.event(token, target)
        self.assertEqual(self.app.status()['sessions'], [])
        voice.dispatch(self.app, {'action': 'team-toggle'})
        self.assertEqual(queued, [])
        self.app.refresh_labels()
        self.assertEqual(len(queued), 1)
        queued.pop()()
        original = voice.build_labels
        def labels(*args):
            self.assertFalse(self.app.lock._is_owned())
            return original(*args)
        with patch.object(voice, 'build_labels', side_effect=labels):
            self.assertIn('Workspace', self.app.status()
                          ['sessions'][0]['label'])
        self.assertTrue(json.loads(
            (self.runtime / 'selection.json').read_text())['show_team'])

    def test_ready_first_non_team_and_sticky_selection(self):
        child = self.target(team_child=True)
        child_token = self.attach(child)
        self.assertIsNone(self.app.token)
        self.assertTrue(self.event(child_token, child))
        self.assertIsNone(self.app.token)
        ordinary = self.target(pane='w1:p2', activation=2)
        token = self.attach(ordinary)
        self.assertIsNone(self.app.token)
        self.assertTrue(self.event(token, ordinary))
        self.assertEqual(self.app.token, token)
        self.app.unregister(token)
        later = self.target(pane='w1:p3', activation=3)
        self.event(self.attach(later), later)
        self.assertIsNone(self.app.token)
        self.assertTrue(
            json.loads((self.runtime / 'selection.json').read_text())[
                'selection_initialized'
            ]
        )

    def test_retry_preserves_state_and_socket(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.pending = 'held text'
        cancelled = self.app.input_cancelled
        self.assertEqual(
            self.app.attach(target), {'token': token, 'state': 'ready'}
        )
        self.assertEqual(self.app.pending, 'held text')
        self.assertIs(self.app.input_cancelled, cancelled)
        with socket.socket(socket.AF_UNIX) as client:
            client.connect(target['adapter_socket'])

    def test_selected_replacement_resets_all_state_and_retires_old(self):
        old = self.target()
        old_token = self.attach(old)
        self.event(old_token, old)
        cancelled = self.app.input_cancelled
        self.app.pending, self.app.draft = 'retained secret', True
        self.app.reply, self.app.turns = 'old reply', {'old-turn'}
        new = self.target(session='conversation-2', activation=2)
        token = self.attach(new)
        self.assertTrue(cancelled.is_set())
        self.assertEqual(self.app.token, token)
        self.assertEqual(self.app.target, self.app.sessions[token]['target'])
        self.assertEqual(self.app.thread, 'conversation-2')
        self.assertFalse(self.app.draft)
        self.assertIsNone(self.app.pending)
        self.assertIsNone(self.app.reply)
        self.assertEqual(self.app.turns, set())
        self.assertFalse(self.app.input_cancelled.is_set())
        self.assertEqual(self.app.retained_dictation.text, 'retained secret')
        self.assertEqual(self.app.attach(old), {'state': 'superseded'})
        for kind in ('ready', 'shutdown', 'reply', 'heartbeat'):
            self.assertFalse(self.event(old_token, old, kind, turn='old'))
        with self.assertRaises(RuntimeError):
            self.app.select(old_token)
        with self.assertRaises(RuntimeError):
            self.app.stage('hello', token)
        saved = (self.runtime / 'selection.json').read_text()
        self.assertNotIn('retained secret', saved)
        self.assertEqual(json.loads(saved)['thread'], 'conversation-2')
        self.assertTrue(self.event(token, new))

    def test_invalid_attach_is_quiet_and_non_destructive(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        cancelled = self.app.input_cancelled
        result = self.app.attach(
            dict(target, adapter_socket=str(self.runtime / 'control.sock'))
        )
        self.assertNotIn('token', result)
        self.assertEqual(self.app.token, token)
        self.assertFalse(cancelled.is_set())
        self.assertIsNone(self.app.error)

    def test_expiry_and_explicit_detach_keep_reconnect_not_selection_theft(
        self,
    ):
        target = self.target()
        with patch.object(voice.time, 'monotonic', return_value=100):
            token = self.attach(target)
            self.event(token, target)
        self.app.monitor_attachments(now=114)
        self.assertEqual(self.app.token, token)
        self.app.monitor_attachments(now=115)
        self.assertIsNone(self.app.token)
        self.assertIsNotNone(self.app.reconnect_pane)
        self.assertFalse(self.event(token, target, 'heartbeat'))
        retry = self.attach(target)
        self.assertIsNone(self.app.token)
        self.assertTrue(self.event(retry, target))
        self.assertEqual(self.app.token, retry)
        self.assertFalse(self.app.detach(retry, dict(target, activation=9)))
        self.assertTrue(self.app.detach(retry, target))
        self.assertIsNone(self.app.token)
        self.assertTrue(Path(target['adapter_socket']).exists())
        self.assertEqual(self.app.attach(target), {'state': 'superseded'})

    def test_dead_restore_retains_policy_and_managed_live_restore_connects(
        self,
    ):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        restarted = voice.Controller(
            self.runtime, self.terminal, self.audio, lambda *a: None
        )
        self.assertTrue(restarted.restore())
        self.assertEqual(
            restarted.sessions[token]['connection_state'], 'connecting'
        )
        with patch.object(voice, 'process_start', return_value=None):
            self.assertFalse(restarted.restore())
        self.assertTrue(restarted.selection_initialized)
        self.assertIsNone(restarted.token)

    def test_async_revision_mismatch_revalidates_different_process(self):
        first = self.target()
        original = self.app._validate_attachment
        entered, release = threading.Event(), threading.Event()
        calls = []

        def validation(target):
            result = original(target)
            if target is first:
                calls.append(1)
                if len(calls) == 1:
                    entered.set()
                    self.assertTrue(release.wait(3))
            return result

        results = []
        with patch.object(
            self.app, '_validate_attachment', side_effect=validation
        ):
            thread = threading.Thread(
                target=lambda: results.append(self.app.attach(first))
            )
            thread.start()
            self.assertTrue(entered.wait(3))
            newer = self.target(activation=2)
            winner = self.attach(newer)
            release.set()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(results, [{'state': 'superseded'}])
        self.assertIn(winner, self.app.sessions)

    def test_different_process_late_validation_must_repeat_foreground_lookup(
        self,
    ):
        import subprocess

        child = subprocess.Popen(
            [sys.executable, '-c', 'import time; time.sleep(30)']
        )
        self.addCleanup(lambda: (child.terminate(), child.wait(timeout=3)))
        first = self.target()
        newer = self.target(pid=child.pid)
        original = self.app._validate_attachment
        entered, release = threading.Event(), threading.Event()
        calls, results = [], []

        def validate(target):
            if target is first:
                calls.append(1)
                if len(calls) > 1:
                    raise ValueError('Old PID is no longer foreground')
                result = original(target)
                entered.set()
                self.assertTrue(release.wait(3))
                return result
            return original(target)

        with patch.object(
            self.app, '_validate_attachment', side_effect=validate
        ):
            thread = threading.Thread(
                target=lambda: results.append(self.app.attach(first))
            )
            thread.start()
            self.assertTrue(entered.wait(3))
            winner = self.attach(newer)
            release.set()
            thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(calls), 2)
        self.assertNotIn('token', results[0])
        self.assertEqual(list(self.app.sessions), [winner])

    def test_ready_handshakes_are_serialized_for_first_selection(self):
        targets = [self.target(pane=f'w1:p{i}', activation=i) for i in (1, 2)]
        tokens = [self.attach(target) for target in targets]
        barrier = threading.Barrier(2)
        original = self.app._validate_attachment

        def validate(target):
            result = original(target)
            barrier.wait(timeout=3)
            return result

        with patch.object(
            self.app, '_validate_attachment', side_effect=validate
        ):
            threads = [
                threading.Thread(target=self.event, args=(token, target))
                for token, target in zip(tokens, targets)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(3)
                self.assertFalse(thread.is_alive())
        selected = self.app.token
        self.assertIn(selected, tokens)
        other = tokens[0] if selected == tokens[1] else tokens[1]
        self.event(other, targets[tokens.index(other)])
        self.assertEqual(self.app.token, selected)

    def test_authoritative_empty_snapshot_purges_disconnected_record(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.detach(token, target)
        with patch.object(
            voice.Herdr, 'request', side_effect=RuntimeError('unavailable')
        ):
            self.app.refresh_reconnect()
        self.assertIsNotNone(self.app.reconnect_pane)
        with patch.object(
            voice.Herdr, 'request', return_value={'snapshot': {'panes': []}}
        ):
            self.app.refresh_reconnect()
        self.assertIsNone(self.app.reconnect_pane)

    def test_stop_and_status_return_while_admission_validation_is_blocked(self):
        target = self.target()
        entered, release = threading.Event(), threading.Event()
        original = self.app._validate_attachment

        def validate(incoming):
            entered.set()
            self.assertTrue(release.wait(3))
            return original(incoming)

        results = []
        with patch.object(
            self.app, '_validate_attachment', side_effect=validate
        ):
            worker = threading.Thread(
                target=lambda: results.append(self.app.attach(target))
            )
            worker.start()
            self.assertTrue(entered.wait(3))
            self.assertEqual(self.app.pending_admissions, 1)
            self.app.status()
            self.app.stop()
            with self.app.lock:
                self.app.attachments_closed = True
            release.set()
            worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(results, [{'state': 'superseded'}])
        self.assertEqual(self.app.pending_admissions, 0)

    def test_restored_heartbeat_requests_fresh_ready_acknowledgement(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.assertTrue(self.app.restore())
        self.assertFalse(self.event(token, target, 'heartbeat'))
        self.assertEqual(
            self.app.attach(target), {'token': token, 'state': 'connecting'}
        )
        self.assertTrue(self.event(token, target))
        self.assertTrue(self.event(token, target, 'heartbeat'))

    def test_new_herdr_instance_never_resurrects_selected_pane(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        Path(target['socket']).unlink()
        replacement = socket.socket(socket.AF_UNIX)
        self.addCleanup(replacement.close)
        replacement.bind(target['socket'])
        new = self.target(activation=2)
        new_token = self.attach(new)
        self.event(new_token, new)
        self.assertIsNone(self.app.token)
        self.assertIsNone(self.app.reconnect_pane)

    def test_missing_bridge_probe_disables_delivery_before_lease_expiry(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.terminal.alive = False
        with self.assertRaises(RuntimeError):
            self.app.stage('must not arrive', token)
        self.assertIsNone(self.app.token)
        self.assertNotIn(token, self.app.sessions)
        self.assertEqual(self.terminal.text, [])

    def test_pid_reuse_is_a_new_registration_not_an_idempotent_retry(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        new = self.target(session='different-process')
        with patch.object(
            voice, 'process_start', return_value='different-start-ticks'
        ):
            replacement = self.attach(new)
        self.assertNotEqual(token, replacement)
        self.assertEqual(self.app.target['start'], 'different-start-ticks')
        self.assertNotIn(token, self.app.sessions)
        self.assertFalse(self.event(token, target))

    def test_lookup_must_confirm_full_pane_identity(self):
        target = self.target()
        with patch.object(
            voice.Herdr,
            'request',
            return_value={
                'process_info': {
                    'pane_id': 'different-pane',
                    'foreground_processes': [
                        {'pid': target['pid'], 'name': 'pi'}
                    ],
                }
            },
        ):
            self.assertNotIn('token', self.app.attach(target))

    def test_foreground_validation_failure_and_process_start_recheck(self):
        target = self.target()
        with patch.object(
            voice.Herdr,
            'request',
            return_value={'process_info': {'foreground_processes': []}},
        ):
            self.assertNotIn('token', self.app.attach(target))
        actual = voice.process_start(os.getpid())
        with patch.object(
            voice, 'process_start', side_effect=[actual, 'reused']
        ):
            self.assertNotIn('token', self.app.attach(target))
        self.assertEqual(self.app.sessions, {})

    def test_reconcile_only_fresh_catalogue_can_forget_reconnect_destination(
        self,
    ):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.detach(token, target)
        pane = self.app.reconnect_pane
        self.app.reconcile_panes(
            pane.socket_path,
            pane.socket_device,
            pane.socket_inode,
            {pane.pane_id},
        )
        self.assertEqual(self.app.reconnect_pane, pane)
        self.app.reconcile_panes(
            pane.socket_path, pane.socket_device, pane.socket_inode, set()
        )
        self.assertIsNone(self.app.reconnect_pane)
        self.assertTrue(self.app.selection_initialized)

    def test_late_transcription_after_replacement_is_recoverable_not_delivered(
        self,
    ):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.audio.transcribe_release.clear()
        created, captures = threading.Event(), []
        original_capture = self.audio.start_capture

        def capture(path):
            result = original_capture(path)
            captures.append(result)
            created.set()
            return result

        with patch.object(self.audio, 'start_capture', side_effect=capture):
            self.app.record()
            worker = self.app.worker
            self.assertTrue(created.wait(2))
            captures[0].close()
            self.assertTrue(self.audio.transcribe_started.wait(2))
        new = self.target(activation=2, session='new-conversation')
        self.attach(new)
        self.audio.transcribe_release.set()
        worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertIsNotNone(self.app.retained_dictation)
        self.assertEqual(self.app.retained_dictation.source_token, token)
        self.assertIsNone(self.app.pending)
        self.assertEqual(self.terminal.text, [])

    def test_explicit_cancel_blocks_late_recovery(self):
        target = self.target()
        cancelled = threading.Event()
        cancelled.recovery_revision = self.app.recovery_revision
        cancelled.voice_target_lost = True
        cancelled.set()
        self.app.recover_discard()
        self.assertFalse(
            self.app.retain_dictation('late text', 'old', target, cancelled)
        )

    def test_explicit_pi_launcher_reconciles_existing_managed_pane(self):
        target = self.target()
        old = self.attach(target)
        self.event(old, target)
        legacy = {
            key: self.app.target[key]
            for key in ('pane', 'socket', 'pid', 'start', 'harness')
        }
        legacy['adapter_socket'] = str(self.runtime / 'legacy-pi.sock')
        self.app.register('launcher', legacy)
        self.assertEqual(list(self.app.sessions), ['launcher'])
        self.assertEqual(self.app.token, 'launcher')
        self.assertTrue(Path(target['adapter_socket']).exists())
        self.assertEqual(self.app.attach(target), {'state': 'superseded'})

    def test_recovery_cli_actions_do_not_start_a_launcher(self):
        for action in ('recover-copy', 'recover-stage', 'recover-discard'):
            with patch.object(
                voice, 'call', return_value={}
            ) as call, patch.object(voice, 'launch') as launch:
                self.assertEqual(voice.main([action]), 0)
                call.assert_called_once_with({'action': action})
                launch.assert_not_called()

    def test_explicit_legacy_selection_cannot_be_stolen(self):
        self.app.register(
            'legacy', dict(pane='other', socket='legacy.sock', pid=1, start='2')
        )
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.assertEqual(self.app.token, 'legacy')

    def test_native_transport_carries_generation_and_rejects_replaced_endpoint(
        self,
    ):
        target = self.target()
        token = self.attach(target)
        managed = self.app.sessions[token]['target']
        listener = self.listeners[target['adapter_socket']]
        received = []

        def respond():
            for _ in range(2):
                connection, _ = listener.accept()
                with connection, connection.makefile('rb') as reader:
                    request = json.loads(reader.readline())
                    received.append(request)
                    status = {
                        key: target[key]
                        for key in (
                            'bridge_id',
                            'activation',
                            'pid',
                            'session',
                            'harness',
                        )
                    }
                    connection.sendall(
                        json.dumps(
                            {'ok': True, 'result': dict(status, ready=True)}
                        ).encode()
                        + b'\n'
                    )

        server = threading.Thread(target=respond, daemon=True)
        server.start()
        transport = voice.PiTerminal()
        transport.insert_guarded(managed, 'native text', threading.Event())
        server.join(3)
        self.assertFalse(server.is_alive())
        self.assertEqual(
            [row['command'] for row in received], ['status', 'stage']
        )
        self.assertTrue(
            all(
                row['bridge_id'] == target['bridge_id']
                and row['activation'] == 1
                for row in received
            )
        )
        with patch.object(
            transport,
            'request',
            return_value=dict(target, bridge_id='old', ready=True),
        ):
            with self.assertRaises(RuntimeError):
                transport.validate_target(managed)
        Path(target['adapter_socket']).unlink()
        Path(target['adapter_socket']).write_text('unrelated file')
        with self.assertRaises(RuntimeError):
            transport.request(managed, 'submit')
        self.assertEqual(
            Path(target['adapter_socket']).read_text(), 'unrelated file'
        )

    def test_recovery_uncertain_stage_must_not_be_pasted_twice(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.app.select(token)
        self.app.retain_dictation('uncertain text', token, target)
        with patch.object(
            self.terminal,
            'insert',
            side_effect=voice.DeliveryUncertain('lost reply'),
        ):
            with self.assertRaises(voice.DeliveryUncertain):
                self.app.recover_stage()
        self.assertIsNotNone(self.app.retained_dictation)
        with self.assertRaises(RuntimeError):
            self.app.recover_stage()
        self.assertEqual(self.terminal.text, [])
        self.app.recover_discard()
        self.assertTrue(self.app.retain_dictation('next', token, target))
        self.assertTrue(self.app.recover_stage())

    def test_recovery_is_explicit_bounded_private_and_never_submits(self):
        target = self.target()
        token = self.attach(target)
        self.event(token, target)
        self.assertTrue(self.app.retain_dictation('secret text', token, target))
        self.assertFalse(self.app.retain_dictation('overwrite', token, target))
        self.assertNotIn('secret text', json.dumps(self.app.status()))
        with self.assertRaises(RuntimeError):
            self.app.record()
        with patch.object(
            voice.subprocess, 'run', side_effect=OSError('no clipboard')
        ):
            with self.assertRaises(RuntimeError):
                self.app.recover_copy()
        self.assertIsNotNone(self.app.retained_dictation)
        with patch.object(voice.subprocess, 'run') as copy:
            self.app.recover_copy()
            self.assertEqual(copy.call_args.args[0], ['wl-copy'])
            self.assertEqual(copy.call_args.kwargs['input'], 'secret text')
        with self.assertRaises(RuntimeError):
            self.app.recover_stage()  # Auto-selection is not recovery consent.
        self.app.select(token)
        self.assertTrue(self.app.recover_stage())
        self.assertEqual(self.terminal.text, ['secret text'])
        self.assertEqual(self.terminal.keys, [])
        self.assertIsNone(self.app.retained_dictation)
        self.assertFalse(
            self.app.retain_dictation('é' * MAX_RETAINED_BYTES, token, target)
        )
        self.assertTrue(self.app.retain_dictation('more', token, target))
        self.app.recover_discard()
        self.assertIsNone(self.app.retained_dictation)
