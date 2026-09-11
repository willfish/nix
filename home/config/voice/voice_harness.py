"""Native Pi editor transport for the local voice controller."""

import json
import socket
from pathlib import Path

from pi_attachments import validate_endpoint, event_matches
from voice_errors import DeliveryUncertain


class PiTerminal:

    @staticmethod
    def validate_endpoint(target):
        if not target.get('managed'):
            return
        try:
            start = (
                Path(f"/proc/{target['pid']}/stat")
                .read_text()
                .rsplit(')', 1)[1]
                .split()[19]
            )
            observed = validate_endpoint(
                Path(target['adapter_socket']).parent,
                target['adapter_socket'],
                target['pid'],
                target['bridge_id'],
            )
            if start != target['start'] or observed != (
                target['adapter_device'],
                target['adapter_inode'],
            ):
                raise ValueError('Pi bridge instance changed')
        except (OSError, ValueError, KeyError, IndexError) as exc:
            raise RuntimeError('Selected Pi bridge is unavailable') from exc

    def request(self, target, command, **fields):
        self.validate_endpoint(target)
        request = {
            'token': target.get('token'), 'session': target.get('session'),
            'command': command, **fields,
        }
        if target.get('managed'):
            request.update(
                {
                    key: target[key]
                    for key in ('bridge_id', 'activation', 'pid', 'harness')
                }
            )
        if not request['token'] or not request['session']:
            raise RuntimeError('Pi voice session is not bound yet')
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(2)
            try:
                sock.connect(target['adapter_socket'])
            except (OSError, KeyError) as exc:
                raise RuntimeError(
                    'Selected Pi session is unavailable'
                ) from exc
            try:
                sock.sendall(json.dumps(request).encode() + b'\n')
                with sock.makefile('rb') as incoming:
                    response = json.loads(incoming.readline(256 * 1024))
                if (
                    not isinstance(response, dict)
                    or type(response.get('ok')) is not bool
                ):
                    raise ValueError('Invalid Pi response')
            except (OSError, ValueError) as exc:
                error = (
                    DeliveryUncertain if command != 'status' else RuntimeError
                )
                raise error(
                    'Could not confirm Pi delivery; check its prompt'
                ) from exc
        try:
            self.validate_endpoint(target)
        except RuntimeError as exc:
            if command != 'status':
                raise DeliveryUncertain(
                    'Pi bridge changed during delivery; check its prompt'
                ) from exc
            raise
        if not response['ok']:
            error = (
                DeliveryUncertain if response.get('uncertain') else RuntimeError
            )
            raise error(response.get('error', 'Pi voice command failed'))
        result = response.get('result')
        if not isinstance(result, dict):
            error = DeliveryUncertain if command != 'status' else RuntimeError
            raise error('Invalid Pi voice acknowledgement')
        return result

    def validate_target(self, target):
        status = self.request(target, 'status')
        if (
            status.get('session') != target.get('session')
            or status.get('pid') != target.get('pid')
            or (target.get('managed') and not event_matches(target, status))
        ):
            raise RuntimeError('Pi session changed; rebind voice')
        return status

    def validate(self, target):
        status = self.validate_target(target)
        if not status.get('ready'):
            raise RuntimeError('Pi is busy or waiting for an interaction')
        return status

    @staticmethod
    def check_cancelled(cancelled):
        if cancelled is not None and cancelled.is_set():
            raise RuntimeError('Voice delivery was cancelled')

    def insert_guarded(self, target, text, cancelled):
        self.check_cancelled(cancelled)
        self.validate(target)
        self.check_cancelled(cancelled)
        self.request(target, 'stage', text=text)

    def insert(self, target, text):
        self.insert_guarded(target, text, None)

    def submit_guarded(self, target, cancelled, allow_edited=False):
        self.check_cancelled(cancelled)
        self.validate(target)
        self.check_cancelled(cancelled)
        fields = {'allow_edited': True} if allow_edited else {}
        self.request(target, 'submit', **fields)

    def submit(self, target):
        self.submit_guarded(target, None)
