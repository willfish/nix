import importlib.util
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'home/config/herdr/grok-model.py'
spec = importlib.util.spec_from_file_location('grok_model', SCRIPT)
reporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reporter)


class GrokModelTests(unittest.TestCase):
    def test_labels(self):
        payload = {'model': {'id': 'grok-test'}, 'effort': {'level': 'high'}}
        self.assertEqual(
            reporter.display_label(payload),
            'grok · high · grok-test',
        )
        self.assertEqual(
            reporter.display_label({}),
            'grok · ? · model unknown',
        )
        dirty = {'model': {'id': '\nfoo\x1b\x7f'}}
        self.assertEqual(
            reporter.display_label(dirty),
            'grok · ? · foo',
        )
        label = reporter.display_label({
            'model': {'id': '界' * 100},
            'effort': {'level': 'low'},
        })
        self.assertEqual(len(label), 80)
        self.assertTrue(label.startswith('grok · low · '))
        self.assertTrue(label.endswith('…'))

    def test_silent_outside_herdr_and_with_bad_input(self):
        for value in ('invalid', 'null', '[]', '{}'):
            stdin = io.StringIO(value)
            with patch.dict('os.environ', {}, clear=True), \
                    patch('sys.stdin', stdin), \
                    patch('sys.stdout', new_callable=io.StringIO) as output:
                reporter.main()
                self.assertEqual(output.getvalue(), '')

    def test_bad_payload_is_ignored_inside_herdr(self):
        for value in ('invalid', 'null', '[]'):
            env = {
                'HERDR_ENV': '1',
                'HERDR_PANE_ID': 'w1:p1',
                'HERDR_SOCKET_PATH': '/fake',
            }
            with patch.dict('os.environ', env, clear=True), \
                    patch('sys.stdin', io.StringIO(value)), \
                    patch.object(reporter.socket, 'socket') as connect:
                reporter.main()
                connect.assert_not_called()

    def test_socket_metadata_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'socket')
            env = {
                'HERDR_ENV': '1',
                'HERDR_SOCKET_PATH': path,
                'HERDR_PANE_ID': 'w1:p1',
            }
            received = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(path)
                server.listen(1)
                server.settimeout(2)

                def receive():
                    conn, _ = server.accept()
                    with conn:
                        line = conn.makefile('rb').readline()
                        received.append(json.loads(line))
                        conn.sendall(b'{"result":{}}\n')

                worker = threading.Thread(target=receive)
                worker.start()
                stdin = io.StringIO(json.dumps({
                    'model': {'id': 'test'},
                    'effort': {'level': 'high'},
                }))
                with patch.dict('os.environ', env, clear=True), \
                        patch('sys.stdin', stdin), \
                        patch('sys.stdout',
                              new_callable=io.StringIO) as output:
                    reporter.main()
                    self.assertEqual(output.getvalue(), '')
                worker.join(3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(received[0]['method'], 'pane.report_metadata')
                params = received[0]['params']
                self.assertEqual(params['pane_id'], 'w1:p1')
                self.assertEqual(params['agent'], 'grok')
                self.assertEqual(params['source'], 'user:grok-model')
                self.assertEqual(params['ttl_ms'], 15000)
                self.assertEqual(params['display_agent'], 'grok · high · test')
                self.assertNotIn('state', params)
                self.assertNotIn('title', params)

    def test_unavailable_server_fails_open(self):
        env = {
            'HERDR_ENV': '1',
            'HERDR_PANE_ID': 'w1:p1',
            'HERDR_SOCKET_PATH': '/nonexistent/herdr.sock',
        }
        with patch.dict('os.environ', env, clear=True), \
                patch('sys.stdin', io.StringIO('{}')), \
                patch('sys.stdout', new_callable=io.StringIO) as output:
            reporter.main()
            self.assertEqual(output.getvalue(), '')


if __name__ == '__main__':
    unittest.main()
