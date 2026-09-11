#!/usr/bin/env python3
"""Isolated transport for the real controller, without audio or service
startup.
"""
import argparse
import json
import os
from pathlib import Path
import socketserver
import sys
import threading

sys.path.insert(
    0, str(Path(__file__).resolve().parents[2] / 'home/config/voice'))
import voice_controller as voice

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', required=True)
parser.add_argument('--pane', default='offline-no-pane')
parser.add_argument('--herdr-socket', default='offline-no-socket')
parser.add_argument('--hold-first', action='store_true')
args = parser.parse_args()
rows, errors, reads, notices, audio_calls = [], [], [], [], []
release = threading.Event()
if not args.hold_first:
    release.set()
first = None
row_lock = threading.Lock()
original_request = voice.Herdr.request


def guarded_request(self, target, *command):
    assert target['pane'] == args.pane, 'Foreign Herdr pane'
    assert target['socket'] == args.herdr_socket, 'Foreign Herdr socket'
    assert command in (
        ('pane', 'process-info', '--pane', args.pane),
        ('api', 'snapshot'),
    ), f'Forbidden Herdr operation: {command}'
    result = original_request(self, target, *command)
    reads.append({'command': command, 'result': result})
    return result


voice.Herdr.request = guarded_request


class SilentAudio:
    def stop(self):
        pass

    def status(self):
        return {}

    def __getattr__(self, name):
        audio_calls.append(name)
        raise AssertionError(f'Unexpected audio operation: {name}')


runtime = Path(args.runtime)
runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
app = voice.Controller(runtime, voice.AgentTerminal(), SilentAudio(),
                       lambda *notice: notices.append(notice))
assert app.engines is None
app.restore()
restored = [{'token': token, 'target': entry['target'],
             'state': entry['connection_state']}
            for token, entry in app.sessions.items()]


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        global first
        try:
            request = json.loads(self.rfile.readline(1024 * 1024))
            action = request.get('action')
            if action == 'fixture-release':
                release.set()
                response = {'ok': True}
            elif action == 'fixture-state':
                with app.lock:
                    response = {
                        'ok': True,
                        'rows': list(rows),
                        'errors': list(errors),
                        'reads': list(reads),
                        'audio_calls': list(audio_calls),
                        'restored': restored,
                        'sessions': [
                            {
                                'token': token,
                                'target': entry['target'],
                                'ready': entry['connection_state'] == 'ready',
                                'heartbeat_at': entry['heartbeat_at'],
                            }
                            for token, entry in app.sessions.items()
                        ],
                    }
            else:
                with row_lock:
                    rows.append(request)
                    if action == 'attach' and first is None:
                        first = request.get('target', {}).get('bridge_id')
                if (
                    action == 'attach'
                    and first
                    and request.get('target', {}).get('bridge_id') == first
                ):
                    if not release.wait(90):
                        raise RuntimeError('Held attachment was not released')
                response = {'ok': True, **voice.dispatch(app, request)}
        except Exception as exc:
            errors.append(f'{type(exc).__name__}: {exc}')
            response = {'ok': False, 'error': str(exc)}
        self.wfile.write(json.dumps(response).encode() + b'\n')


class Server(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True


path = runtime / 'control.sock'
path.unlink(missing_ok=True)
with Server(str(path), Handler) as server:
    os.chmod(path, 0o600)
    print('ready', flush=True)
    server.serve_forever()
