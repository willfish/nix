"""Opt-in offline TUI test: PI_HERDR_TEST_BIN=pi python3 -m unittest ..."""

import json
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import tempfile
import threading
import time
import unittest


@unittest.skipUnless(
    os.environ.get('PI_HERDR_TEST_BIN'),
    'requires installed Pi',
)
class PiRuntimeTests(unittest.TestCase):
    def test_real_tui_reports_live_model_effort_and_clears_on_exit(self):
        prefix = 'pi-herdr-runtime-'
        with tempfile.TemporaryDirectory(prefix=prefix) as directory:
            root = Path(directory)
            agent = root / 'agent'
            agent.mkdir()
            settings = {'enableInstallTelemetry': False}
            (agent / 'settings.json').write_text(json.dumps(settings))
            models = [{
                'id': model,
                'name': model,
                'reasoning': True,
                'input': ['text'],
                'contextWindow': 32000,
                'maxTokens': 4096,
                'cost': {
                    'input': 0,
                    'output': 0,
                    'cacheRead': 0,
                    'cacheWrite': 0,
                },
            } for model in ('model-one', 'model-two')]
            providers = {
                'fixture': {
                    'baseUrl': 'http://127.0.0.1:1/v1',
                    'api': 'openai-completions',
                    'apiKey': 'offline-test-only',
                    'models': models,
                },
            }
            payload = json.dumps({'providers': providers})
            (agent / 'models.json').write_text(payload)
            fixture = root / 'fixture.js'
            fixture.write_text('''export default function(pi) {
              pi.on('session_start', (_event, ctx) => {
                setTimeout(() => pi.setThinkingLevel('high'), 200);
                setTimeout(async () => {
                  const model = ctx.modelRegistry.find(
                    'fixture', 'model-two');
                  await pi.setModel(model);
                }, 400);
                setTimeout(() => ctx.shutdown(), 900);
              });
            }''')
            socket_path = str(root / 'herdr.sock')
            requests = []
            stop = threading.Event()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(socket_path)
                server.listen(8)
                server.settimeout(0.1)

                def receive():
                    while not stop.is_set():
                        try:
                            conn, _ = server.accept()
                        except socket.timeout:
                            continue
                        with conn:
                            conn.settimeout(1)
                            request = json.loads(conn.makefile('rb').readline())
                            requests.append(request)
                            conn.sendall(b'{"result":{}}\n')

                worker = threading.Thread(target=receive)
                worker.start()
                master, slave = pty.openpty()
                parents = Path(__file__).resolve().parents[1]
                extension = parents / 'home/config/pi/extensions/herdr-model.ts'
                child = subprocess.Popen([
                    os.environ['PI_HERDR_TEST_BIN'],
                    '--offline', '--no-extensions',
                    '--no-skills', '--no-prompt-templates',
                    '--no-context-files',
                    '--provider', 'fixture',
                    '--model', 'model-one',
                    '--thinking', 'medium',
                    '-e', str(extension), '-e', str(fixture),
                ], cwd=root, env={
                    **os.environ,
                    'PI_CODING_AGENT_DIR': str(agent),
                    'HERDR_ENV': '1',
                    'HERDR_SOCKET_PATH': socket_path,
                    'HERDR_PANE_ID': 'w1:p1',
                    'CAPTURE_PROMPTS': '0',
                    'TERM': 'xterm-256color',
                }, stdin=slave, stdout=slave, stderr=slave)
                os.close(slave)
                output = bytearray()
                try:
                    deadline = time.monotonic() + 15
                    while child.poll() is None and time.monotonic() < deadline:
                        if select.select([master], [], [], 0.1)[0]:
                            try:
                                output.extend(os.read(master, 65536))
                            except OSError:
                                break
                    child.wait(timeout=3)
                    decoded = output.decode(errors='replace')
                    self.assertEqual(child.returncode, 0, decoded)
                    labels = [
                        r['params'].get('display_agent') for r in requests
                    ]
                    self.assertIn(
                        'pi · medium · model-one',
                        labels,
                        decoded,
                    )
                    self.assertIn('pi · high · model-one', labels)
                    self.assertIn('pi · high · model-two', labels)
                    last = requests[-1]['params']
                    self.assertTrue(last['clear_display_agent'])
                    self.assertTrue(all(
                        r['method'] == 'pane.report_metadata'
                        for r in requests
                    ))
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
                    os.close(master)
                    stop.set()
                    worker.join(2)
