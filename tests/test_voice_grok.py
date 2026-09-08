import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'home/config/voice'
spec = importlib.util.spec_from_file_location(
    'grok_voice_hook', SOURCE / 'grok_voice_hook.py'
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GrokTests(unittest.TestCase):
    def event(self, name, **fields):
        return module.events_for({
            'hookEventName': name, 'sessionId': 'session-a',
            'cwd': '/tmp/project', **fields,
        })

    def test_root_session_registration(self):
        self.assertEqual(self.event('session_start'), [{
            'harness': 'grok', 'session': 'session-a', 'type': 'session',
            'cwd': '/tmp/project',
        }])

    def test_subagent_events_never_change_root_voice(self):
        for name in ('session_end', 'stop', 'stop_cancelled', 'stop_failure'):
            self.assertEqual(self.event(name, subagentType='explore'), [])
        self.assertEqual(self.event('subagent_start'), [])
        self.assertEqual(self.event('subagent_stop'), [])

    def test_completion_is_provisional_and_correlated_to_prompt(self):
        result = self.event(
            'stop', promptId='turn-a', lastAssistantMessage='Finished.'
        )
        self.assertEqual(result, [{
            'harness': 'grok', 'session': 'session-a',
            'cwd': '/tmp/project', 'turn': 'turn-a',
            'type': 'reply', 'text': 'Finished.', 'provisional': True,
        }])

    def test_teardown_stop_never_repeats_last_reply(self):
        for reason in ('shutdown', 'channel_closed'):
            self.assertEqual(self.event(
                'stop', reason=reason, lastAssistantMessage='Old reply'
            ), [])

    def test_cancel_and_failure_never_speak_partial_or_error_text(self):
        for name in ('stop_cancelled', 'stop_failure'):
            result = self.event(
                name, promptId='turn-a', lastAssistantMessage='Partial/error'
            )
            self.assertEqual(result[0]['type'], 'settled')
            self.assertEqual(result[0]['turn'], 'turn-a')
            self.assertNotIn('text', result[0])

    def test_busy_and_idle_notifications(self):
        self.assertEqual(
            self.event('user_prompt_submit', promptId='a')[0]['type'], 'busy'
        )
        self.assertEqual(
            self.event('notification', notificationType='idle_prompt')[0]
            ['type'], 'settled'
        )
        self.assertEqual(self.event('notification',
                                   notificationType='task_complete'), [])

    def test_missing_session_and_non_text_reply_are_ignored(self):
        self.assertEqual(module.events_for({'hookEventName': 'stop'}), [])
        self.assertEqual(self.event('stop', lastAssistantMessage={}), [])
        self.assertEqual(self.event('stop', lastAssistantMessage='  '), [])

    def test_real_hook_is_quiet_and_scoped_to_voice_launcher(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = str(Path(tmp) / 'control.sock')
            received = []
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path)
                server.listen()
                server.settimeout(2)

                def receive():
                    connection, _ = server.accept()
                    with connection:
                        with connection.makefile('rb') as stream:
                            received.append(json.loads(stream.readline()))
                        connection.sendall(b'{"ok":true}\n')

                worker = threading.Thread(target=receive)
                worker.start()
                environment = dict(
                    os.environ, AGENT_VOICE_TOKEN='voice-token',
                    AGENT_VOICE_KIND='grok', AGENT_VOICE_SOCKET=path,
                )
                result = subprocess.run(
                    [sys.executable, str(SOURCE / 'grok_voice_hook.py')],
                    input=json.dumps({
                        'hookEventName': 'session_start',
                        'sessionId': 'session-a',
                    }), env=environment, capture_output=True,
                    text=True, timeout=2,
                )
                worker.join(2)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertEqual(result.stderr, '')
            self.assertEqual(received[0]['action'], 'harness-event')
            self.assertEqual(received[0]['token'], 'voice-token')
            for kind in ('pi', ''):
                environment['AGENT_VOICE_KIND'] = kind
                result = subprocess.run(
                    [sys.executable, str(SOURCE / 'grok_voice_hook.py')],
                    input='not json', env=environment,
                    capture_output=True, text=True, timeout=2,
                )
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout + result.stderr, '')

    def test_session_registration_retries_for_at_most_two_seconds(self):
        for accepted_after in (4, 1000):
            with self.subTest(accepted_after=accepted_after):
                with tempfile.TemporaryDirectory() as tmp:
                    path = str(Path(tmp) / 'control.sock')
                    received = []
                    stopping = threading.Event()
                    with socket.socket(socket.AF_UNIX) as server:
                        server.bind(path)
                        server.listen()
                        server.settimeout(0.1)

                        def receive():
                            while not stopping.is_set():
                                try:
                                    connection, _ = server.accept()
                                except TimeoutError:
                                    continue
                                with connection:
                                    with connection.makefile('rb') as stream:
                                        received.append(json.loads(
                                            stream.readline()
                                        ))
                                    connection.sendall(json.dumps({
                                        'ok': True,
                                        'accepted': len(received)
                                        >= accepted_after,
                                    }).encode() + b'\n')

                        worker = threading.Thread(target=receive)
                        worker.start()
                        started = time.monotonic()
                        try:
                            result = subprocess.run(
                                [sys.executable,
                                 str(SOURCE / 'grok_voice_hook.py')],
                                input=json.dumps({
                                    'hookEventName': 'session_start',
                                    'sessionId': 'session-a',
                                }), env=dict(
                                    os.environ, AGENT_VOICE_TOKEN='token',
                                    AGENT_VOICE_KIND='grok',
                                    AGENT_VOICE_SOCKET=path,
                                ), capture_output=True, text=True,
                                timeout=3,
                            )
                            elapsed = time.monotonic() - started
                        finally:
                            stopping.set()
                            worker.join(1)
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout + result.stderr, '')
                    self.assertGreater(len(received), 1)
                    self.assertLess(elapsed, 2.2)
                    if accepted_after == 4:
                        self.assertEqual(len(received), 4)

    def test_malformed_hook_and_missing_service_cannot_block_grok(self):
        environment = dict(
            os.environ, AGENT_VOICE_TOKEN='voice-token',
            AGENT_VOICE_KIND='grok', AGENT_VOICE_SOCKET='/missing/voice.sock',
        )
        for payload in ('not json', json.dumps({
            'hookEventName': 'stop', 'sessionId': 'a',
            'lastAssistantMessage': 'Done.',
        })):
            result = subprocess.run(
                [sys.executable, str(SOURCE / 'grok_voice_hook.py')],
                input=payload, env=environment,
                capture_output=True, text=True, timeout=2,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout + result.stderr, '')


if __name__ == '__main__':
    unittest.main()
