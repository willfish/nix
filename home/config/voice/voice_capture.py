"""Capture PipeWire PCM with explicit readiness and finalized WAV ownership."""

import array
from contextlib import suppress
import math
import os
import select
import signal
import subprocess
import tempfile
import threading
import time
import wave


RATE = 16000
SAMPLE_LIMIT = RATE * 180
FRAME = 320
VOICED_RMS = 200
SILENCE_FRAMES = 30
FORCE_FRAMES = 30 * RATE // FRAME
LOOKBACK_FRAMES = 2 * RATE // FRAME
MIN_VOICED_FRAMES = 8


def write_pcm16_wav(path, samples, rate=RATE):
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(
            samples.tobytes() if isinstance(samples, array.array) else samples
        )


class SpeechChunker:
    """Split PCM on pauses, or at 30 seconds if speech does not pause."""

    def __init__(
        self,
        frame=FRAME,
        silence_frames=SILENCE_FRAMES,
        force_frames=FORCE_FRAMES,
        lookback_frames=LOOKBACK_FRAMES,
    ):
        self.frame = frame
        self.silence_frames = silence_frames
        self.force_frames = force_frames
        self.lookback_frames = lookback_frames
        self._pending = array.array("h")

    def push(self, samples):
        if samples:
            self._pending.extend(samples)
        emitted = []
        while True:
            chunk = self._cut(final=False)
            if chunk is None:
                return emitted
            emitted.append(chunk)

    def flush(self):
        return self._cut(final=True)

    def _frames(self):
        count = len(self._pending) // self.frame
        voiced = []
        for index in range(count):
            start = index * self.frame
            window = self._pending[start:start + self.frame]
            energy = sum(sample * sample for sample in window)
            rms = math.sqrt(energy / len(window))
            voiced.append(rms >= VOICED_RMS)
        return voiced

    def _take(self, end_sample, drop_through=None):
        chunk = self._pending[:end_sample]
        keep = end_sample if drop_through is None else drop_through
        self._pending = self._pending[keep:]
        return chunk or None

    def _cut(self, final):
        voiced = self._frames()
        if not voiced:
            if final and self._pending:
                chunk, self._pending = self._pending, array.array("h")
                return chunk
            return None
        seen = 0
        silence = 0
        for index, is_voiced in enumerate(voiced):
            if is_voiced:
                seen += 1
                silence = 0
                continue
            silence += 1
            if seen >= MIN_VOICED_FRAMES and silence >= self.silence_frames:
                speech_end = (index + 1 - silence) * self.frame
                drop_through = (index + 1) * self.frame
                return self._take(speech_end, drop_through)
        if len(voiced) >= self.force_frames:
            split = self.force_frames
            start = max(0, split - self.lookback_frames)
            for index in range(split - 1, start - 1, -1):
                if not voiced[index]:
                    split = index + 1
                    break
            if split < MIN_VOICED_FRAMES:
                split = self.force_frames
            return self._take(split * self.frame)
        if final:
            chunk, self._pending = self._pending, array.array("h")
            return chunk or None
        return None


class PipeWireCapture:
    def __init__(self, path, command=None, target=None, chunker=None):
        self.level = 0.0
        self._clipped_until = 0.0
        self.started_at = None
        self.error = None
        self._ready = threading.Event()
        self._done = threading.Event()
        self._stopping = threading.Event()
        self._close_lock = threading.Lock()
        self._stderr = tempfile.TemporaryFile()
        self._wav = None
        self._process = None
        self._returncode = None
        self._frames = 0
        self._chunker = chunker or SpeechChunker()
        self._chunks = []
        self._chunk_lock = threading.Lock()
        self._progress = threading.Event()
        try:
            self._wav = wave.open(str(path), "wb")
            self._wav.setnchannels(1)
            self._wav.setsampwidth(2)
            self._wav.setframerate(16000)
            self._process = subprocess.Popen(
                command or [
                    "pw-record", "--raw", "--rate=16000", "--channels=1",
                    "--format=s16", f"--sample-count={SAMPLE_LIMIT}",
                    *(["--target", target] if target else []), "-",
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=self._stderr,
            )
            self._reader = threading.Thread(target=self._read, daemon=True)
            self._reader.start()
        except BaseException:
            if self._process is not None:
                with suppress(OSError, subprocess.TimeoutExpired):
                    self._process.kill()
                    self._process.wait(0.5)
                self._process.stdout.close()
            if self._wav is not None:
                with suppress(OSError, wave.Error):
                    self._wav.close()
                with suppress(OSError):
                    os.unlink(path)
            self._stderr.close()
            raise

    def _read(self):
        pending = b""
        last_samples = time.monotonic()
        try:
            while True:
                readable, _, _ = select.select(
                    [self._process.stdout], [], [], 0.1
                )
                if not readable:
                    if self._process.poll() is not None:
                        break
                    if (self.started_at is not None
                            and not self._stopping.is_set()
                            and time.monotonic() - last_samples > 3):
                        self.error = "Microphone audio stream stalled"
                        self._process.kill()
                        break
                    continue
                data = os.read(self._process.stdout.fileno(), 8192)
                if not data:
                    break
                data = pending + data
                size = len(data) - len(data) % 2
                data, pending = data[:size], data[size:]
                if not data:
                    continue
                last_samples = time.monotonic()
                self._wav.writeframesraw(data)
                self._frames += len(data) // 2
                samples = array.array("h", data)
                if any(abs(sample) >= 32760 for sample in samples):
                    self._clipped_until = time.monotonic() + 1.5
                self.level = min(1.0, math.sqrt(
                    sum(sample * sample for sample in samples) / len(samples)
                ) / 32768)
                for chunk in self._chunker.push(samples):
                    self._queue_chunk(chunk)
                if self.started_at is None:
                    self.started_at = time.monotonic()
                    self._ready.set()
        except Exception as exc:
            self.error = f"Microphone capture failed ({type(exc).__name__})"
            with suppress(OSError):
                self._process.kill()
        finally:
            try:
                try:
                    self._wav.close()
                except Exception:
                    self.error = self.error or "Could not finalize audio WAV"
                self._process.stdout.close()
                try:
                    result = self._process.wait(0.2)
                except subprocess.TimeoutExpired:
                    self.error = self.error or "Microphone stream disconnected"
                    self._process.kill()
                    result = self._process.wait(0.5)
                # pw-cat 1.6 returns 1 unless playback drained, including
                # successful recording stops and the recording sample limit.
                # Accept that only after samples and an expected completion,
                # with no diagnostic output or capture/finalization error.
                if (result == 1 and self.started_at is not None
                        and self.error is None
                        and (self._stopping.is_set()
                             or self._frames >= SAMPLE_LIMIT)
                        and os.fstat(self._stderr.fileno()).st_size == 0):
                    result = 0
                self._returncode = result
                stopped = self._stopping.is_set() and result < 0
                if result and not stopped and self.error is None:
                    self.error = (
                        f"Microphone capture exited with status {result}"
                    )
            except Exception as exc:
                self.error = self.error or (
                    f"Microphone cleanup failed ({type(exc).__name__})"
                )
                with suppress(OSError):
                    self._process.kill()
            finally:
                if self._returncode is None:
                    self._returncode = self._process.poll()
                if self.started_at is None and self.error is None:
                    self.error = "Microphone stopped before producing samples"
                remainder = self._chunker.flush()
                if remainder:
                    self._queue_chunk(remainder)
                self._progress.set()
                self._done.set()
                self._ready.set()

    @property
    def clipping(self):
        return time.monotonic() < self._clipped_until

    def wait_ready(self, timeout=5):
        if not self._ready.wait(timeout):
            self.error = "Microphone produced no audio samples before timeout"
            self.close()
        if self.error:
            raise RuntimeError(self.error)
        return self.started_at is not None

    def _queue_chunk(self, samples):
        with self._chunk_lock:
            self._chunks.append(samples)
            self._progress.set()

    def drain_chunks(self):
        with self._chunk_lock:
            chunks = self._chunks
            self._chunks = []
            if not chunks and not self._done.is_set():
                self._progress.clear()
            return chunks

    def wait_progress(self, timeout=None):
        return self._progress.wait(timeout)

    def wait(self, timeout=None):
        if not self._done.wait(timeout):
            raise subprocess.TimeoutExpired(self._process.args, timeout)
        return self._returncode

    def poll(self):
        return self._returncode if self._done.is_set() else None

    def send_signal(self, sig):
        self._stopping.set()
        with suppress(ProcessLookupError):
            self._process.send_signal(sig)

    def terminate(self):
        self.send_signal(signal.SIGTERM)

    def kill(self):
        self.send_signal(signal.SIGKILL)

    def close(self):
        with self._close_lock:
            if self._stderr.closed:
                return
            for sig, timeout in (
                (signal.SIGINT, 0.3), (signal.SIGTERM, 0.3),
                (signal.SIGKILL, 0.3),
            ):
                if self._done.is_set():
                    break
                self.send_signal(sig)
                self._done.wait(timeout)
            self._reader.join(0.2)
            self._stderr.close()
