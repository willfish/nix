#!/usr/bin/env python3
"""Observe Grok lifecycle hooks without affecting its control flow."""

import json
import os
import socket
import sys
import time


HOOK_EVENTS = (
    'SessionStart', 'UserPromptSubmit', 'Stop', 'StopFailure',
    'StopCancelled', 'Notification', 'SessionEnd',
)


def events_for(payload):
    if not isinstance(payload, dict) or payload.get('subagentType'):
        return []
    session = payload.get('sessionId')
    if not isinstance(session, str) or not session:
        return []
    event = {
        'harness': 'grok', 'session': session,
        'cwd': payload.get('cwd', ''),
    }
    turn = payload.get('promptId')
    if isinstance(turn, str) and turn:
        event['turn'] = turn
    name = payload.get('hookEventName')
    if name == 'session_start':
        event['type'] = 'session'
    elif name == 'user_prompt_submit':
        event['type'] = 'busy'
    elif name == 'stop':
        if payload.get('reason') in ('shutdown', 'channel_closed'):
            return []
        text = payload.get('lastAssistantMessage')
        if not isinstance(text, str) or not text.strip():
            return []
        # Other Stop hooks can still extend this turn. Herdr readiness must
        # confirm completion before the controller speaks this candidate.
        event.update(type='reply', text=text[:32768], provisional=True)
    elif name in ('stop_failure', 'stop_cancelled'):
        event['type'] = 'settled'
    elif name == 'notification':
        if payload.get('notificationType') != 'idle_prompt':
            return []
        event['type'] = 'settled'
    elif name == 'session_end':
        event['type'] = 'shutdown'
    else:
        return []
    return [event]


def main():
    token = os.environ.get('AGENT_VOICE_TOKEN')
    path = os.environ.get('AGENT_VOICE_SOCKET')
    if (
        not token or not path
        or os.environ.get('AGENT_VOICE_KIND') != 'grok'
    ):
        return 0
    try:
        raw = sys.stdin.buffer.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            return 0
        for event in events_for(json.loads(raw)):
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(min(0.75, deadline - time.monotonic()))
                    sock.connect(path)
                    sock.sendall(json.dumps({
                        'action': 'harness-event', 'token': token,
                        'event': event,
                    }).encode() + b'\n')
                    data = b''
                    while b'\n' not in data and len(data) <= 65536:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise TimeoutError('Voice deadline expired')
                        sock.settimeout(min(0.75, remaining))
                        chunk = sock.recv(65536 - len(data) + 1)
                        if not chunk:
                            raise ValueError('Voice response was incomplete')
                        data += chunk
                    response = json.loads(data)
                if (
                    event['type'] != 'session'
                    or not isinstance(response, dict)
                    or not response.get('ok')
                    or response.get('accepted') is not False
                    or deadline - time.monotonic() <= 0.1
                ):
                    break
                time.sleep(0.1)
    except (OSError, ValueError, TypeError):
        # Observe-only: no stdout, error decision, or service startup.
        pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
