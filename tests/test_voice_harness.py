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
