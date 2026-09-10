"""Silent Grok status-line command: report live model/effort to Herdr only.

Grok supplies model.id and effort.level and reruns on selection changes.
Printing nothing keeps Grok's previously disabled status row hidden.
"""

import json
import os
import socket
import sys
import time


def clean(value):
    if not isinstance(value, str):
        return ''
    return ''.join(
        c for c in value if ord(c) >= 32 and not 127 <= ord(c) <= 159
    ).strip()


def display_label(payload):
    model = payload.get('model')
    effort = payload.get('effort')
    model_id = clean(model.get('id')) if isinstance(model, dict) else ''
    level = clean(effort.get('level')) if isinstance(effort, dict) else ''
    label = f"grok · {level or '?'} · {model_id or 'model unknown'}"
    return label if len(label) <= 80 else label[:79] + '…'


def main():
    path = os.environ.get('HERDR_SOCKET_PATH')
    pane = os.environ.get('HERDR_PANE_ID')
    if os.environ.get('HERDR_ENV') != '1' or not path or not pane:
        return
    try:
        # Never retain or forward the session payload, transcript path or cwd.
        payload = json.loads(sys.stdin.read(65536))
        if not isinstance(payload, dict):
            return
        sequence = time.time_ns() // 1000
        request = {
            'id': f'user:grok-model:{sequence}',
            'method': 'pane.report_metadata',
            'params': {
                'pane_id': pane,
                'source': 'user:grok-model',
                'agent': 'grok',
                'display_agent': display_label(payload),
                'seq': sequence,
                'ttl_ms': 15000,
            },
        }
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(0.5)
            client.connect(path)
            client.sendall((json.dumps(request) + '\n').encode())
            client.recv(4096)
    except (OSError, ValueError):
        # Cosmetic integration: no stdout, no blocking or failing the harness.
        pass


if __name__ == '__main__':
    main()
