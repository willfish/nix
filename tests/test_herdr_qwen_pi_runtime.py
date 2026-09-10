"""Opt-in offline TUI test of the actual Qwen Pi launcher and metadata."""

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
    os.environ.get('QWEN_PI_HERDR_TEST_BIN'),
    'requires installed qwen-pi',
)
class QwenPiRuntimeTests(unittest.TestCase):
    def test_launcher_reports_model_effort_changes_and_clears_on_exit(self):
        with tempfile.TemporaryDirectory(prefix='qwen-herdr-') as directory:
            root = Path(directory)
            fixture = root / 'fixture.js'
            fixture.write_text('''export default function(pi) {
              pi.on('session_start', (_event, ctx) => {
                setTimeout(() => pi.setThinkingLevel('low'), 300);
                setTimeout(() => ctx.shutdown(), 1200);
              });
            }''')
            socket_path = str(root / 'herdr.sock')
            requests = []
            stop = threading.Event()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(socket_path)
                server.listen(16)
                server.settimeout(0.1)

                def receive():
                    while not stop.is_set():
                        try:
                            conn, _ = server.accept()
                        except socket.timeout:
                            continue
                        with conn:
                            conn.settimeout(1)
                            try:
                                line = conn.makefile('rb').readline()
                                if line:
                                    requests.append(json.loads(line))
                                    conn.sendall(b'{"result":{}}\n')
                            except (OSError, ValueError):
                                continue

                worker = threading.Thread(target=receive)
                worker.start()
                master, slave = pty.openpty()
                child = subprocess.Popen([
                    os.environ['QWEN_PI_HERDR_TEST_BIN'],
                    '--no-session', '--thinking', 'medium',
                    '-e', str(fixture),
                ], cwd=root, env={
                    **os.environ,
                    'HERDR_ENV': '1',
                    'HERDR_SOCKET_PATH': socket_path,
                    'HERDR_PANE_ID': 'w1:p1',
                    'CAPTURE_PROMPTS': '0',
                    'TERM': 'xterm-256color',
                }, stdin=slave, stdout=slave, stderr=slave)
                os.close(slave)
                output = bytearray()
                try:
                    deadline = time.monotonic() + 20
                    while child.poll() is None and time.monotonic() < deadline:
                        if select.select([master], [], [], 0.1)[0]:
                            try:
                                output.extend(os.read(master, 65536))
                            except OSError:
                                break
                    child.wait(timeout=3)
                    self.assertEqual(child.returncode, 0)
                    metadata = [
                        r['params'] for r in requests
                        if r.get('method') == 'pane.report_metadata'
                        and r.get('params', {}).get('source') == 'user:pi-model'
                    ]
                    # Do not dump terminal output, which can include local
                    # profile data. Only inspect the display metadata contract.
                    self.assertTrue(metadata, 'no model metadata received')
                    labels = [m.get('display_agent', '') for m in metadata]
                    medium = [
                        s for s in labels if s.startswith('pi · medium · ')
                    ]
                    self.assertTrue(medium, labels)
                    model = medium[0].split(' · ', 2)[2]
                    self.assertTrue(model.startswith('qwen'), model)
                    self.assertIn(f'pi · low · {model}', labels)
                    self.assertTrue(metadata[-1]['clear_display_agent'])
                    self.assertTrue(all(m['agent'] == 'pi' for m in metadata))
                    self.assertTrue(all(
                        m['pane_id'] == 'w1:p1' for m in metadata
                    ))
                    self.assertEqual(
                        [m['seq'] for m in metadata],
                        sorted({m['seq'] for m in metadata}),
                    )
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
                    os.close(master)
                    stop.set()
                    worker.join(2)
