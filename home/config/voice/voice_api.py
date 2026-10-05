#!/usr/bin/env python3
"""Loopback Deepgram REST subset backed by Whisper and audio.cpp.

Only prerecorded WAV recognition and linear16/WAV synthesis are supported.
Inference engines remain separate, on-demand processes managed by the frontend.
"""

import argparse
import io
import json
import secrets
import threading
import urllib.error
import urllib.parse
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LIMIT = 16 * 1024 * 1024
OUTPUT_LIMIT = 32 * 1024 * 1024


class APIError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


class Backend:
    def __init__(self, config):
        self.config = config

        # Never inherit HTTP proxies or forward redirects to another host.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args):
                return None

        self.http = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect()
        )
        for name in ("stt_url", "tts_url"):
            url = urllib.parse.urlsplit(config[name])
            if (
                url.scheme != "http"
                or url.hostname != "127.0.0.1"
                or url.username
                or url.password
            ):
                raise ValueError(f"{name} must be a loopback HTTP URL")
        self.lock = threading.Lock()

    def request(self, url, body, content_type):
        request = urllib.request.Request(
            url, body, {"Content-Type": content_type}
        )
        try:
            with self.http.open(request, timeout=85) as response:
                result = response.read(OUTPUT_LIMIT + 1)
        except (OSError, urllib.error.URLError) as exc:
            raise APIError(
                502, "Local inference engine unavailable or request failed"
            ) from exc
        if len(result) > OUTPUT_LIMIT:
            raise APIError(502, "Inference response exceeds size limit")
        return result

    def listen(self, body, query):
        try:
            with wave.open(io.BytesIO(body)) as audio:
                if (
                    audio.getcomptype() != "NONE"
                    or audio.getnchannels() != 1
                    or audio.getsampwidth() != 2
                ):
                    raise ValueError()
        except (wave.Error, EOFError, ValueError) as exc:
            raise APIError(400, "Expected mono PCM16 WAV audio") from exc
        boundary = secrets.token_hex(16)
        fields = {
            "response_format": "json",
            "temperature": "0",
            "language": query.get("language", ["en"])[0],
            "prompt": ", ".join(query.get("keyterm", [])),
        }
        parts = [
            (
                f"--{boundary}\r\nContent-Disposition: form-data; "
                f'name="{key}"\r\n\r\n{value}\r\n'
            ).encode()
            for key, value in fields.items()
        ]
        parts.extend(
            [
                (
                    f"--{boundary}\r\nContent-Disposition: form-data; "
                    'name="file"; filename="dictation.wav"\r\n'
                    "Content-Type: audio/wav\r\n\r\n"
                ).encode(),
                body,
                f"\r\n--{boundary}--\r\n".encode(),
            ]
        )
        result = json.loads(
            self.request(
                self.config["stt_url"],
                b"".join(parts),
                f"multipart/form-data; boundary={boundary}",
            )
        )
        if not isinstance(result, dict) or not isinstance(
            result.get("text"), str
        ):
            raise APIError(502, "Inference engine returned no transcript")
        return "application/json", json.dumps(
            {
                "results": {
                    "channels": [
                        {"alternatives": [{"transcript": result["text"]}]}
                    ]
                }
            }
        ).encode()

    def speak(self, body, query):
        for key, expected in (
            ("encoding", "linear16"),
            ("container", "wav"),
            ("sample_rate", "24000"),
        ):
            if query.get(key, [expected]) != [expected]:
                raise APIError(400, f"Only {key}={expected} is supported")
        try:
            payload = json.loads(body)
        except (ValueError, UnicodeError) as exc:
            raise APIError(400, "Expected JSON containing text") from exc
        if (
            not isinstance(payload, dict)
            or not isinstance(payload.get("text"), str)
            or not payload["text"].strip()
        ):
            raise APIError(400, "Expected nonempty text")
        model = query.get("model", ["samantha"])[0]
        voices = self.config.get("voices", {"samantha": {}})
        if model not in voices:
            raise APIError(400, "Unknown local voice model")
        # Reference paths come only from trusted config, never from HTTP input.
        request = {
            **voices[model],
            "model": self.config.get("tts_model", "pi-voice"),
            "input": payload["text"],
            "language": "English",
        }
        result = self.request(
            self.config["tts_url"],
            json.dumps(request).encode(),
            "application/json",
        )
        try:
            with wave.open(io.BytesIO(result)) as audio:
                if (
                    audio.getcomptype(),
                    audio.getsampwidth(),
                    audio.getframerate(),
                ) != ("NONE", 2, 24000):
                    raise ValueError()
                frames = audio.readframes(audio.getnframes())
                if len(frames) != audio.getnframes() * audio.getnchannels() * 2:
                    raise ValueError()
        except (wave.Error, EOFError, ValueError) as exc:
            raise APIError(
                502, "Inference engine returned invalid WAV"
            ) from exc
        return "audio/wav", result


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # Do not log transcripts, request URLs or voice references.

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def handle(self):
        try:
            super().handle()
        except OSError:
            pass  # Client cancellation must not log input or a traceback.

    def do_GET(self):
        self.respond(
            200 if self.path == "/health" else 404,
            "application/json",
            b'{"status":"ready"}' if self.path == "/health" else b"{}",
        )

    def do_POST(self):
        backend = self.server.backend
        acquired = False
        try:
            # No browser access, chunked bodies or proxy destinations.
            if self.headers.get("Origin") or self.headers.get(
                "Transfer-Encoding"
            ):
                raise APIError(
                    403,
                    "Browser and transfer-encoded requests are not supported",
                )
            route = urllib.parse.urlsplit(self.path)
            if route.path not in ("/v1/listen", "/v1/speak"):
                raise APIError(404, "Unknown endpoint")
            lengths = self.headers.get_all("Content-Length", [])
            if (
                len(lengths) != 1
                or not lengths[0].isascii()
                or not lengths[0].isdecimal()
            ):
                raise APIError(411, "A single Content-Length is required")
            length = int(lengths[0])
            if not 0 < length <= LIMIT:
                raise APIError(413, "Request exceeds size limit or is empty")
            acquired = backend.lock.acquire(blocking=False)
            if not acquired:
                raise APIError(429, "Local inference is busy")
            body = self.rfile.read(length)
            if len(body) != length:
                raise APIError(400, "Incomplete request body")
            operation = (
                backend.listen if route.path == "/v1/listen" else backend.speak
            )
            content_type, result = operation(
                body, urllib.parse.parse_qs(route.query)
            )
            self.respond(200, content_type, result)
        except APIError as exc:
            self.respond(
                exc.status,
                "application/json",
                json.dumps(
                    {"err_code": "VOICE_ERROR", "err_msg": exc.message}
                ).encode(),
            )
        except (ValueError, UnicodeError):
            self.respond(
                502,
                "application/json",
                b'{"err_code":"INVALID_UPSTREAM_RESPONSE"}',
            )
        except (OSError, TimeoutError):
            self.close_connection = True
        finally:
            if acquired:
                backend.lock.release()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, backend):
        self.backend = backend
        super().__init__(address, Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    with open(args.config, encoding="utf-8") as source:
        config = json.load(source)
    with Server(
        ("127.0.0.1", config.get("port", 8180)), Backend(config)
    ) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()
