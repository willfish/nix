"""Native Pi editor transport for the local voice controller."""

import json
import socket

from voice_errors import DeliveryUncertain


class PiTerminal:
    def request(self, target, command, **fields):
        request = {
            'token': target.get('token'), 'session': target.get('session'),
            'command': command, **fields,
        }
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
                if not isinstance(response, dict) or 'ok' not in response:
                    raise ValueError('Invalid Pi response')
            except (OSError, ValueError) as exc:
                error = (
                    DeliveryUncertain if command != 'status' else RuntimeError
                )
                raise error(
                    'Could not confirm Pi delivery; check its prompt'
                ) from exc
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

    def submit_guarded(self, target, cancelled):
        self.check_cancelled(cancelled)
        self.validate(target)
        self.check_cancelled(cancelled)
        self.request(target, 'submit')

    def submit(self, target):
        self.submit_guarded(target, None)
