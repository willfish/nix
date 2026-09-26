import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'home/config/voice'
sys.path.insert(0, str(SOURCE))
from voice_harness import PiTerminal
from voice_errors import DeliveryUncertain


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = str(Path(self.directory.name) / 'pi.sock')
        self.target = {
            'adapter_socket': self.path, 'token': 'secret',
            'session': 'session-a', 'pid': 123,
        }
        self.requests = []

    def server(self, replies):
        server = socket.socket(socket.AF_UNIX)
        server.bind(self.path)
        server.listen()
        server.settimeout(2)

        def run():
            try:
                for response in replies:
                    connection, _ = server.accept()
                    with connection:
                        request = b''
                        while b'\n' not in request:
                            request += connection.recv(65536)
                        self.requests.append(json.loads(request))
                        if response is not None:
                            connection.sendall(
                                json.dumps(response).encode() + b'\n'
                            )
            except TimeoutError:
                pass
            finally:
                server.close()

        worker = threading.Thread(target=run)
        worker.start()
        self.addCleanup(worker.join, 3)

    def status(self, ready=True, **fields):
        return {'ok': True, 'result': {
            'session': 'session-a', 'pid': 123, 'ready': ready,
            'state': 'idle' if ready else 'working', **fields,
        }}

    def test_validates_live_session_but_allows_recording_while_busy(self):
        self.server([self.status(False), self.status(False)])
        terminal = PiTerminal()
        self.assertEqual(
            terminal.validate_target(self.target)['state'], 'working'
        )
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            terminal.validate(self.target)

    def test_busy_capable_bridge_stages_and_submits_explicitly(self):
        busy = self.status(False, accepts_input=True)
        self.server([busy, busy, busy, busy])
        terminal = PiTerminal()
        terminal.insert(self.target, 'Draft while working')
        terminal.submit(self.target)
        self.assertEqual(
            [request['command'] for request in self.requests],
            ['status', 'stage', 'status', 'submit'],
        )

    def test_blocked_bridge_rejects_stage_and_submit(self):
        blocked = self.status(
            False, accepts_input=False, state='blocked'
        )
        self.server([blocked, blocked])
        terminal = PiTerminal()
        with self.assertRaisesRegex(RuntimeError, 'interaction'):
            terminal.insert(self.target, 'No')
        with self.assertRaisesRegex(RuntimeError, 'interaction'):
            terminal.submit(self.target)
        self.assertEqual(
            [request['command'] for request in self.requests],
            ['status', 'status'],
        )

    def test_busy_submit_lost_ack_remains_uncertain_without_replay(self):
        self.server([self.status(False, accepts_input=True), None])
        with self.assertRaises(DeliveryUncertain):
            PiTerminal().submit(self.target)
        self.assertEqual(
            [request['command'] for request in self.requests],
            ['status', 'submit'],
        )

    def test_rejects_wrong_session_or_process(self):
        self.server([
            self.status(session='session-b'), self.status(pid=456)
        ])
        terminal = PiTerminal()
        for _ in range(2):
            with self.assertRaises(RuntimeError):
                terminal.validate_target(self.target)

    def test_lost_mutation_acknowledgement_cannot_be_retried(self):
        self.server([self.status(), None])
        with self.assertRaises(DeliveryUncertain):
            PiTerminal().insert(self.target, 'hello')
        self.assertEqual(self.requests[-1]['command'], 'stage')

    def test_non_boolean_acknowledgement_is_uncertain(self):
        self.server([self.status(), {'ok': 'unknown', 'result': {}}])
        with self.assertRaises(DeliveryUncertain):
            PiTerminal().insert(self.target, 'hello')
        self.assertEqual(self.requests[-1]['command'], 'stage')

    def test_safe_rejection_is_distinct_from_lost_ack(self):
        self.server([
            self.status(), {'ok': False, 'error': 'Pi is busy'}
        ])
        with self.assertRaisesRegex(RuntimeError, 'busy') as caught:
            PiTerminal().insert(self.target, 'hello')
        self.assertNotIsInstance(caught.exception, DeliveryUncertain)

    def test_cancel_during_validation_prevents_delivery(self):
        cancelled = threading.Event()

        class CancellingTerminal(PiTerminal):
            def validate(self, target):
                cancelled.set()
                return {'state': 'idle'}

        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
            CancellingTerminal().insert_guarded(
                self.target, 'hello', cancelled
            )
        self.assertEqual(self.requests, [])

    def test_submit_uses_native_single_use_command(self):
        self.server([self.status(), self.status(draft=False)])
        PiTerminal().submit(self.target)
        self.assertEqual(self.requests[-1], {
            'token': 'secret', 'session': 'session-a', 'command': 'submit',
        })

    def test_explicit_hotkey_submit_opts_into_current_edited_draft(self):
        self.server([self.status(), self.status(draft=False)])
        PiTerminal().submit_guarded(
            self.target, threading.Event(), allow_edited=True
        )
        self.assertEqual(self.requests[-1], {
            'token': 'secret', 'session': 'session-a', 'command': 'submit',
            'allow_edited': True,
        })


if __name__ == '__main__':
    unittest.main()
