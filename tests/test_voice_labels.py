import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / 'home/config/voice'))
from voice_labels import (SnapshotCache, SnapshotState, SocketKey, build_labels,
                          display_width, parse_snapshot, run_snapshot)


class Queue:
    def __init__(self):
        self.jobs = []

    def submit(self, fn):
        self.jobs.append(fn)

    def run(self):
        self.jobs.pop(0)()


def payload(pane='w1:p1', workspace='dot', tab='shell'):
    return {'workspaces': [{'workspace_id': 'w1', 'label': workspace}],
            'tabs': [{'tab_id': 't1', 'label': tab}],
            'panes': [{'pane_id': pane, 'workspace_id': 'w1', 'tab_id': 't1'}]}


def row(pane='w1:p1', key=None, **kw):
    return dict(token=pane, id='conversation', harness='pi', pane=pane,
                socket_key=key or SocketKey('/a', 1, 2), **kw)


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.queue = Queue()
        self.calls = []
        self.result = payload()
        self.key = SocketKey('/a', 1, 2)
        def runner(key, timeout):
            self.calls.append((key, timeout))
            if isinstance(self.result, Exception):
                raise self.result
            return self.result
        self.cache = SnapshotCache(runner=runner, clock=lambda: self.now,
                                   scheduler=self.queue, max_keys=2)
        self.cache.set_active([self.key])
        self.addCleanup(self.cache.close)

    def test_reads_are_pure_and_single_flight_and_success_ttl(self):
        self.assertEqual(self.cache.read(self.key).outcome, 'unknown')
        self.assertEqual(self.queue.jobs, [])
        self.assertTrue(self.cache.refresh(self.key))
        self.assertFalse(self.cache.refresh(self.key))
        self.assertTrue(self.cache.read(self.key).inflight)
        self.queue.run()
        self.assertTrue(self.cache.read(self.key).authoritative(self.now))
        self.assertFalse(self.cache.refresh(self.key))
        self.now = 3
        self.assertTrue(self.cache.refresh(self.key))
        self.queue.run()
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(self.calls[0][1], 1)

    def test_empty_is_success_and_failures_preserve_stale_not_evidence(self):
        self.cache.refresh(self.key)
        self.queue.run()
        self.now = 3
        self.result = TimeoutError()
        self.cache.refresh(self.key)
        self.queue.run()
        state = self.cache.read(self.key)
        self.assertEqual(state.outcome, 'unavailable')
        self.assertIn('w1:p1', state.panes)
        self.assertFalse(state.authoritative(self.now))
        self.assertFalse(self.cache.refresh(self.key))
        self.now = 6
        self.result = {'panes': [], 'tabs': [], 'workspaces': []}
        self.cache.refresh(self.key)
        self.queue.run()
        self.assertEqual(self.cache.read(self.key).panes, {})
        self.assertTrue(self.cache.read(self.key).authoritative(self.now))

    def test_malformed_negative_ttl_and_immutable_snapshots(self):
        self.result = {'unexpected': []}
        self.cache.refresh(self.key)
        self.queue.run()
        self.assertEqual(self.cache.read(self.key).outcome, 'unavailable')
        self.assertFalse(self.cache.refresh(self.key))
        self.now = 3
        self.result = payload()
        self.cache.refresh(self.key)
        self.queue.run()
        with self.assertRaises(TypeError):
            self.cache.read(self.key).panes['w1:p1']['workspace'] = 'changed'
        self.assertFalse(self.cache.read(self.key).authoritative(6))

    def test_retirement_fences_late_result_and_bounds_pending_work(self):
        self.cache.refresh(self.key)
        self.cache.set_active([])
        self.cache.set_active([self.key])
        self.cache.refresh(self.key)
        self.queue.run()
        self.assertEqual(self.cache.read(self.key).outcome, 'unknown')
        self.queue.run()
        self.assertEqual(self.cache.read(self.key).outcome, 'ok')
        with self.assertRaises(ValueError):
            self.cache.set_active([SocketKey(str(i), 1, i) for i in range(3)])

    def test_runner_checks_socket_instance_and_passes_timeout(self):
        stat = SimpleNamespace(st_dev=1, st_ino=2)
        changed = SimpleNamespace(st_dev=1, st_ino=3)
        result = SimpleNamespace(
            stdout=(
                '{"result": {"snapshot": {"panes": [], "tabs": [], '
                '"workspaces": []}}}'
            )
        )
        with patch('voice_labels.os.stat', return_value=stat), patch(
                'voice_labels.subprocess.run', return_value=result):
            self.assertEqual(run_snapshot(self.key, 1),
                             {'panes': [], 'tabs': [], 'workspaces': []})
        with patch('voice_labels.os.stat', side_effect=[stat, changed]), patch(
                'voice_labels.subprocess.run', return_value=result) as command:
            with self.assertRaisesRegex(OSError, 'instance changed'):
                run_snapshot(self.key, 1)
            self.assertEqual(command.call_args.kwargs['timeout'], 1)
            self.assertEqual(
                command.call_args.kwargs['env']['HERDR_SOCKET_PATH'], '/a')
        with patch('voice_labels.os.stat', return_value=changed), patch(
                'voice_labels.subprocess.run') as command:
            with self.assertRaises(OSError):
                run_snapshot(self.key, 1)
            command.assert_not_called()

    def test_simultaneous_refreshes_only_schedule_once(self):
        gate = threading.Barrier(3)
        def refresh():
            gate.wait()
            self.cache.refresh(self.key)
        workers = [threading.Thread(target=refresh) for _ in range(2)]
        for worker in workers:
            worker.start()
        gate.wait()
        for worker in workers:
            worker.join(2)
            self.assertFalse(worker.is_alive())
        self.assertEqual(len(self.queue.jobs), 1)
        self.queue.run()
        self.assertEqual(len(self.calls), 1)

    def test_blocked_worker_does_not_block_reads_or_other_socket(self):
        entered = threading.Event()
        release = threading.Event()
        other_done = threading.Event()
        second = SocketKey('/b', 1, 3)
        def runner(key, timeout):
            if key == self.key:
                entered.set()
                self.assertTrue(release.wait(2))
            else:
                other_done.set()
            return payload()
        cache = SnapshotCache(runner=runner, max_keys=2)
        try:
            cache.set_active([self.key, second])
            cache.refresh(self.key)
            self.assertTrue(entered.wait(2))
            self.assertTrue(cache.read(self.key).inflight)
            self.assertFalse(cache.refresh(self.key))
            cache.refresh(second)
            self.assertTrue(other_done.wait(2))
        finally:
            release.set()
            cache.close()


class LabelTests(unittest.TestCase):
    def state(self, **kw):
        return SnapshotState(panes=parse_snapshot(payload(**kw)), outcome='ok')

    def test_preferred_and_legacy_unchanged(self):
        entry = row(model={'id': 'gpt-6-astra'}, thinking='medium')
        legacy = {'harness': 'legacy', 'label': 'legacy', 'id': 'old'}
        rows = build_labels(
            [entry, legacy], {entry['socket_key']: self.state()})
        self.assertEqual(rows[0]['label'],
                         'pi · dot · shell · medium · gpt-6-astra')
        self.assertEqual(rows[1], legacy)
        self.assertEqual(rows[0]['id'], 'conversation')
        self.assertNotIn('label', entry)

    def test_named_tab_is_kept_without_workspace_peers(self):
        polling = row('w44:p1H', model='gpt-6-astra', thinking='medium')
        other = row('w4D:p3')
        panes = {
            **parse_snapshot(payload(pane='w44:p1H',
                                     workspace='frontend', tab='polling')),
            **parse_snapshot(payload(pane='w4D:p3', tab='voice')),
        }
        state = SnapshotState(panes=panes)
        for entries in ([polling], [polling, other]):
            with self.subTest(count=len(entries)):
                result = build_labels(entries, {polling['socket_key']: state})
                self.assertEqual(result[0]['label'],
                                 'pi · frontend · polling · medium · '
                                 'gpt-6-astra')
                self.assertNotIn('p1H', result[0]['full_label'])

    def test_different_named_tabs_need_no_pane_suffix(self):
        entries = [row('w44:p1H'), row('w44:p2N')]
        panes = {
            **parse_snapshot(payload(pane='w44:p1H',
                                     workspace='frontend', tab='polling')),
            **parse_snapshot(payload(pane='w44:p2N',
                                     workspace='frontend', tab='stories')),
        }
        result = build_labels(entries, {
            entries[0]['socket_key']: SnapshotState(panes=panes),
        })
        self.assertEqual([entry['label'] for entry in result],
                         ['pi · frontend · polling',
                          'pi · frontend · stories'])

    def test_tab_without_workspace_still_provides_a_name(self):
        entry = row()
        result = build_labels([entry], {
            entry['socket_key']: self.state(workspace='', tab='polling'),
        })
        self.assertEqual(result[0]['label'], 'pi · polling')

    def test_truncated_names_still_get_unique_suffixes(self):
        entries = [row(), row('w1:p2')]
        panes = {
            **parse_snapshot(payload(tab='long tab ' * 10 + 'polling')),
            **parse_snapshot(payload(pane='w1:p2',
                                     tab='long tab ' * 10 + 'stories')),
        }
        result = build_labels(entries, {
            entries[0]['socket_key']: SnapshotState(panes=panes),
        })
        self.assertNotEqual(result[0]['label'], result[1]['label'])
        for entry, suffix in zip(result, ('p1', 'p2')):
            self.assertTrue(entry['label'].endswith(suffix))
            self.assertLessEqual(display_width(entry['label']), 55)

    def test_full_pane_fallback_and_cross_server_discriminator(self):
        entries = [row(), row('w2:p1'), row(key=SocketKey('/b', 2, 3))]
        labels = [r['label'] for r in build_labels(entries, {})]
        self.assertEqual(len(set(labels)), 3)
        self.assertTrue(all('w' in s and ':p1' in s for s in labels))
        self.assertEqual(labels, [r['label']
                         for r in build_labels(entries, {})])

    def test_duplicate_tabs_use_full_panes_when_short_pane_collides(self):
        entries = [row(), row('w2:p1')]
        panes = dict(parse_snapshot(payload()))
        panes.update(parse_snapshot(payload(pane='w2:p1')))
        result = build_labels(
            entries, {entries[0]['socket_key']: SnapshotState(panes=panes)})
        self.assertIn('shell', result[0]['full_label'])
        self.assertIn('w1:p1', result[0]['label'])
        self.assertIn('w2:p1', result[1]['label'])

    def test_unicode_controls_suffix_and_long_name_collisions(self):
        entries = [row(team_child=True, selected=True), row('w1:p2')]
        panes = dict(parse_snapshot(payload(workspace='界e\u0301\n\x1b' * 80)))
        panes.update(parse_snapshot(
            payload(pane='w1:p2', workspace='界e\u0301\n\x1b' * 80)))
        result = build_labels(
            entries, {entries[0]['socket_key']: SnapshotState(panes=panes)})
        self.assertNotEqual(result[0]['label'], result[1]['label'])
        for r in result:
            self.assertLessEqual(display_width(r['label']), 55)
            self.assertNotIn('\n', r['full_label'])
            self.assertNotIn('\x1b', r['full_label'])
        self.assertIn('team', result[0]['label'])
        self.assertIn('p1', result[0]['label'])
        self.assertIn('p2', result[1]['label'])
        self.assertEqual(display_width('界e\u0301'), 3)

    def test_renamed_pane_is_shown_without_replacing_the_tab(self):
        data = payload(workspace='dot', tab='1 voice')
        data['panes'][0]['label'] = 'review'
        data['panes'][0]['title'] = 'ignored when label is set'
        data['panes'][0]['terminal_title'] = 'π - .dotfiles'
        entry = row(model='grok-4.7', thinking='medium')
        label = build_labels([entry], {
            entry['socket_key']: SnapshotState(
                panes=parse_snapshot(data), outcome='ok'
            ),
        })[0]['label']
        self.assertEqual(
            label, 'pi · dot · 1 voice · review · medium · grok-4.7'
        )
        self.assertNotIn('.dotfiles', label)
        data['panes'][0]['label'] = None
        data['panes'][0]['title'] = 'notes'
        renamed = build_labels([entry], {
            entry['socket_key']: SnapshotState(
                panes=parse_snapshot(data), outcome='ok'
            ),
        })[0]['label']
        self.assertIn('notes', renamed)
        self.assertNotIn('review', renamed)

    def test_pane_name_matching_the_tab_is_not_repeated(self):
        data = payload()
        data['panes'][0]['label'] = 'shell'
        entry = row()
        label = build_labels([entry], {
            entry['socket_key']: SnapshotState(
                panes=parse_snapshot(data), outcome='ok'
            ),
        })[0]['label']
        self.assertEqual(label.count('shell'), 1)

    def test_display_agent_fallback_is_pure(self):
        data = payload()
        data['panes'][0]['display_agent'] = 'pi · high · provider/model'
        state = SnapshotState(panes=parse_snapshot(data))
        self.assertEqual(
            build_labels([row()], {row()['socket_key']: state})[0]['label'],
            'pi · dot · shell · high · provider/model',
        )


if __name__ == '__main__':
    unittest.main()
