import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / 'home/config/voice'))
from voice_engines import EngineManager, systemctl_active


class Queue:
    def __init__(self):
        self.jobs = []

    def submit(self, fn):
        self.jobs.append(fn)

    def run(self):
        self.jobs.pop(0)()

    def drain(self):
        while self.jobs:
            self.run()


class Timer:
    def __init__(self, fn):
        self.fn = fn
        self.cancelled = False

    def cancel(self):
        self.cancelled = True

    def fire(self):
        if not self.cancelled:
            self.fn()


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.commands = []
        self.queue = Queue()
        self.readies = Queue()
        self.timers = []
        def later(delay, fn):
            timer = Timer(fn)
            self.timers.append((delay, timer))
            return timer
        self.manager = EngineManager(
            runner=lambda engine, command, timeout: self.commands.append(
                (engine, command)
            ),
            readiness=lambda engine, timeout: True,
            clock=lambda: self.now,
            scheduler=self.queue,
            readiness_scheduler=self.readies,
            call_later=later,
        )
        self.addCleanup(self.manager.close)

    def ready(self, lease):
        self.queue.drain()
        self.readies.drain()
        self.assertIsNone(lease.wait(0))

    def test_restart_adopts_active_services_with_fresh_idle_grace(self):
        self.now = 10000
        queries = []
        self.manager.reconcile_startup(
            lambda engine, timeout: queries.append((engine, timeout))
            or engine == 'stt'
        )
        self.manager.reconcile_startup(
            lambda *a: self.fail('adoption runs only once'))
        self.assertEqual(queries, [])
        self.queue.drain()
        self.assertTrue(self.manager.state('stt').running)
        self.assertFalse(self.manager.state('tts').running)
        self.assertEqual(self.commands, [])
        self.now += 899
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands, [])
        self.now += 1
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands, [('stt', 'stop')])

    def test_systemctl_activity_query_is_bounded_and_distinguishes_unknown(
        self,
    ):
        for state, expected in [
            ('active', True),
            ('activating', True),
            ('inactive', False),
            ('failed', False),
            ('', None),
        ]:
            with self.subTest(state=state), patch(
                    'voice_engines.subprocess.run',
                    return_value=SimpleNamespace(stdout=state)) as run:
                if expected is None:
                    with self.assertRaisesRegex(
                        RuntimeError, 'unknown ActiveState'
                    ):
                        systemctl_active('stt', 3)
                else:
                    self.assertIs(systemctl_active('stt', 3), expected)
                self.assertEqual(run.call_args.kwargs['timeout'], 3)
                self.assertTrue(run.call_args.kwargs['check'])
                self.assertNotIn('start', run.call_args.args[0])

    def test_adoption_query_failure_retains_cleanup_and_admission_fences(self):
        self.manager.set_population(1)
        def unknown(engine, timeout):
            raise TimeoutError('query unavailable')
        self.manager.reconcile_startup(unknown)
        self.manager.set_population(0, pending_admissions=1)
        self.queue.drain()
        self.assertTrue(self.manager.state('stt').running)
        self.assertIsNotNone(self.manager.state('stt').error)
        self.assertEqual(self.commands, [])
        lease = self.manager.acquire('stt')
        self.ready(lease)
        self.manager.set_population(0)
        self.queue.drain()
        self.assertNotIn(('stt', 'stop'), self.commands)
        lease.release()
        self.queue.drain()
        self.assertIn(('stt', 'stop'), self.commands)

    def test_nonblocking_engine_specific_and_long_operations(self):
        lease = self.manager.acquire('stt')
        self.assertFalse(lease.ready.done())
        self.assertEqual(self.commands, [])
        self.ready(lease)
        self.now = 2000
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands, [('stt', 'start')])
        lease.release()
        self.assertEqual(self.manager.state('stt').last_release, 2000)
        self.now = 2899
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(len(self.commands), 1)
        self.now = 2900
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands[-1], ('stt', 'stop'))

    def test_acquire_invalidates_queued_idle_stop(self):
        first = self.manager.acquire('tts')
        self.ready(first)
        first.release()
        self.now = 900
        self.manager.sweep()
        second = self.manager.acquire('tts')
        self.ready(second)
        self.assertEqual(self.commands, [('tts', 'start')])
        second.release()

    def test_stop_in_progress_orders_restart_before_readiness(self):
        entered = threading.Event()
        release = threading.Event()
        calls = []
        def runner(engine, command, timeout):
            calls.append(command)
            if command == 'stop':
                entered.set()
                self.assertTrue(release.wait(2))
        self.manager.runner = runner
        first = self.manager.acquire('stt')
        self.ready(first)
        first.release()
        self.now = 900
        self.manager.sweep()
        worker = threading.Thread(target=self.queue.run)
        worker.start()
        try:
            self.assertTrue(entered.wait(2))
            second = self.manager.acquire('stt')
            self.assertFalse(second.ready.done())
            self.assertEqual(self.manager.state('stt').users, 1)
        finally:
            release.set()
            worker.join(2)
        self.ready(second)
        self.assertEqual(calls, ['start', 'stop', 'start'])
        second.release()

    def test_cancel_loading_and_replace_playback(self):
        first = self.manager.acquire('tts')
        self.queue.drain()
        first.release()
        second = self.manager.acquire('tts')
        self.ready(second)
        self.assertTrue(first.ready.cancelled())
        self.assertEqual(self.manager.state('tts').users, 1)
        self.now = 4000
        self.manager.sweep()
        self.queue.drain()
        self.assertNotIn(('tts', 'stop'), self.commands)
        second.release()
        second.release()
        self.assertEqual(self.manager.state('tts').users, 0)

    def test_last_session_waits_for_work_and_pending_admissions(self):
        self.manager.set_population(1, 0)
        lease = self.manager.acquire('stt')
        self.ready(lease)
        self.manager.set_population(0, 1)
        lease.release()
        self.queue.drain()
        self.assertNotIn(('stt', 'stop'), self.commands)
        self.manager.set_population(0, 0)
        self.queue.drain()
        self.assertEqual(self.commands[-1], ('stt', 'stop'))

    def test_new_registration_fences_zero_session_stop(self):
        self.manager.set_population(1, 0)
        lease = self.manager.acquire('stt')
        self.ready(lease)
        lease.release()
        self.manager.set_population(0, 0)
        self.manager.set_population(1, 0)
        self.queue.drain()
        self.assertNotIn(('stt', 'stop'), self.commands)

    def test_initial_zero_and_warm_completion_release_time(self):
        self.manager.set_population(0, 0)
        self.manager.sweep()
        self.assertEqual(self.queue.jobs, [])
        lease, = self.manager.warm(['stt'], timeout=30)
        self.queue.drain()
        self.assertEqual(self.manager.state('stt').users, 1)
        self.now = 25
        self.readies.drain()
        self.assertTrue(lease.ready.done())
        self.assertEqual(self.manager.state('stt').users, 0)
        self.assertEqual(self.manager.state('stt').last_release, 25)
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands, [('stt', 'start')])

    def test_warm_timeout_and_one_backend_failure_release(self):
        def ready(engine, timeout):
            if engine == 'tts':
                raise RuntimeError('offline')
            return True
        self.manager.readiness = ready
        stt, tts = self.manager.warm(['stt', 'tts'], timeout=30)
        self.queue.drain()
        self.readies.drain()
        self.assertIsNone(stt.wait(0))
        with self.assertRaisesRegex(RuntimeError, 'tts.*offline'):
            tts.wait(0)
        self.assertEqual(self.manager.state('tts').users, 0)
        self.assertEqual(self.manager.state('stt').users, 0)
        timed, = self.manager.warm(['stt'], timeout=10)
        timer = self.timers[-1][1]
        self.now = 10
        timer.fire()
        with self.assertRaises(TimeoutError):
            timed.wait(0)
        self.queue.drain()
        self.readies.drain()
        self.assertEqual(self.manager.state('stt').users, 0)

    def test_start_error_and_context_error_release(self):
        def fail(engine, command, timeout):
            raise OSError('systemctl failed')
        self.manager.runner = fail
        with self.assertRaisesRegex(RuntimeError, 'stt'):
            with self.manager.acquire('stt') as lease:
                self.queue.drain()
                lease.wait(0)
        self.assertEqual(self.manager.state('stt').users, 0)
        self.manager.runner = lambda *args: None
        with self.assertRaises(ValueError):
            with self.manager.acquire('tts') as lease:
                self.ready(lease)
                raise ValueError('playback failed')
        self.assertEqual(self.manager.state('tts').users, 0)

    def test_last_session_closes_during_work_and_stops_only_after_release(self):
        self.manager.set_population(1)
        lease = self.manager.acquire('tts')
        self.ready(lease)
        self.manager.set_population(0)
        self.now = 2000
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands, [('tts', 'start')])
        lease.release()
        self.queue.drain()
        self.assertEqual(self.commands[-1], ('tts', 'stop'))

    def test_cancel_while_start_command_is_blocked(self):
        entered = threading.Event()
        release = threading.Event()
        def runner(*args):
            entered.set()
            if not release.wait(2):
                raise TimeoutError('test barrier')
        self.manager.runner = runner
        self.manager.set_population(1)
        lease = self.manager.acquire('stt')
        worker = threading.Thread(target=self.queue.run)
        worker.start()
        try:
            self.assertTrue(entered.wait(2))
            lease.release()
            self.manager.set_population(0)
            self.assertTrue(lease.ready.cancelled())
        finally:
            release.set()
            worker.join(2)
            self.assertFalse(worker.is_alive())
        self.queue.drain()
        self.assertFalse(self.manager.state('stt').running)
        self.assertEqual(self.manager.state('stt').users, 0)
        self.assertEqual(self.readies.jobs, [])

    def test_pending_admission_invalidates_already_queued_stop(self):
        self.manager.set_population(1)
        lease = self.manager.acquire('stt')
        self.ready(lease)
        lease.release()
        self.manager.set_population(0)
        self.manager.set_population(0, 1)
        self.queue.drain()
        self.assertEqual(self.commands, [('stt', 'start')])
        self.manager.set_population(0)
        self.queue.drain()
        self.assertEqual(self.commands[-1], ('stt', 'stop'))

    def test_release_scheduler_error_does_not_mask_operation_error(self):
        def fail(*args):
            raise OSError('timer failed')
        self.manager.call_later = fail
        with self.assertRaisesRegex(ValueError, 'operation failed'):
            with self.manager.acquire('stt') as lease:
                self.ready(lease)
                raise ValueError('operation failed')
        self.assertEqual(self.manager.state('stt').users, 0)
        self.assertIn('timer failed', self.manager.state('stt').error)
        self.now = 900
        self.manager.sweep()
        self.queue.drain()
        self.assertEqual(self.commands[-1], ('stt', 'stop'))

    def test_default_worker_serializes_commands_while_readiness_blocks(self):
        entered = threading.Event()
        release = threading.Event()
        tts_started = threading.Event()
        calls = []
        def readiness(engine, timeout):
            if engine == 'stt':
                entered.set()
                if not release.wait(2):
                    raise TimeoutError('test barrier')
            return True
        def runner(engine, command, timeout):
            calls.append((engine, command))
            if engine == 'tts':
                tts_started.set()
        manager = EngineManager(runner=runner, readiness=readiness)
        try:
            stt = manager.acquire('stt')
            self.assertTrue(entered.wait(2))
            self.assertEqual(manager.state('stt').users, 1)
            tts = manager.acquire('tts')
            self.assertTrue(tts_started.wait(2))
            self.assertIsNone(tts.wait(2))
            stt.release()
            self.assertTrue(stt.ready.cancelled())
            tts.release()
        finally:
            release.set()
            manager.close()
        self.assertEqual(calls, [('stt', 'start'), ('tts', 'start')])

    def test_stop_error_forces_restart_for_next_caller(self):
        lease = self.manager.acquire('stt')
        self.ready(lease)
        lease.release()
        self.now = 900
        self.manager.runner = lambda *args: (_ for _ in ()
                                             ).throw(OSError('partial stop'))
        self.manager.sweep()
        self.queue.drain()
        self.manager.runner = (
            lambda engine, command, timeout: self.commands.append(
                (engine, command)
            )
        )
        second = self.manager.acquire('stt')
        self.ready(second)
        self.assertEqual(self.commands, [('stt', 'start'), ('stt', 'start')])
        second.release()

    def test_stop_error_keeps_retryable_state(self):
        lease = self.manager.acquire('stt')
        self.ready(lease)
        lease.release()
        self.now = 900
        self.manager.runner = lambda *args: (_ for _ in ()
                                             ).throw(OSError('stop failed'))
        self.manager.sweep()
        self.queue.drain()
        self.assertIn('stop failed', self.manager.state('stt').error)
        self.manager.runner = lambda *args: None
        self.manager.sweep()
        self.queue.drain()
        self.assertFalse(self.manager.state('stt').running)


if __name__ == '__main__':
    unittest.main()
