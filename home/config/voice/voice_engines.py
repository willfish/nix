"""Operation leases with serialized, generation-fenced audio service lifecycle.

No work is started at construction. acquire() never waits for a command or
model.
Hold the lease through capture/transcription/retries or playback, and await its
ready Future only before using the backend. Always release in finally (or with).
Injected submit schedulers must enqueue without blocking; the lifecycle
scheduler must execute FIFO on exactly one worker. readiness(engine, timeout)
must be bounded.
"""

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, replace
import subprocess
import threading
import time


@dataclass
class EngineState:
    users: int = 0
    generation: int = 0
    last_release: float = 0.0
    command: str | None = None
    running: bool = False
    error: str | None = None


def systemctl(engine, command, timeout):
    subprocess.run(
        ['systemctl', '--user', command, f'pi-voice-{engine}.service'],
        check=True,
        capture_output=True,
        timeout=timeout,
    )


def systemctl_active(engine, timeout):
    result = subprocess.run(
        ['systemctl', '--user', 'show', '--property=ActiveState', '--value',
         f'pi-voice-{engine}.service'],
        check=True, capture_output=True, text=True, timeout=timeout)
    state = result.stdout.strip()
    if state in ('inactive', 'failed'):
        return False
    if state in (
        'active',
        'activating',
        'reloading',
        'deactivating',
        'refreshing',
    ):
        return True
    raise RuntimeError(f'{engine}: unknown ActiveState {state!r}')


def _later(delay, fn):
    timer = threading.Timer(delay, fn)
    timer.daemon = True
    timer.start()
    return timer


class EngineLease:
    def __init__(self, manager, engine):
        self._manager, self.engine = manager, engine
        self.ready = Future()
        self._released = False
        self._timer = None

    def wait(self, timeout=None):
        """Wait off the controller thread; raises on cancellation or backend
        failure.
        """
        return self.ready.result(timeout)

    def release(self):
        self._manager._release(self)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


class EngineManager:
    def __init__(self, *, readiness, runner=systemctl, clock=time.monotonic,
                 scheduler=None, readiness_scheduler=None, call_later=_later,
                 idle_timeout=900, readiness_timeout=60, command_timeout=10,
                 engines=('stt', 'tts')):
        self.runner, self.readiness, self.clock = runner, readiness, clock
        self.call_later = call_later
        self.idle_timeout = idle_timeout
        self.readiness_timeout, self.command_timeout = (
            readiness_timeout,
            command_timeout,
        )
        self._own_worker, self._own_readers = (
            scheduler is None,
            readiness_scheduler is None,
        )
        self._worker = scheduler or ThreadPoolExecutor(max_workers=1)
        self._readers = readiness_scheduler or ThreadPoolExecutor(max_workers=4)
        self._lock = threading.RLock()
        engines = tuple(engines)
        unknown = any(engine not in ('stt', 'tts') for engine in engines)
        if not engines or unknown:
            raise ValueError('invalid engines')
        self._states = {engine: EngineState() for engine in engines}
        self._leases = set()
        self._idle_timers = {}
        self._queued_stops = set()
        self._sessions = self._admissions = self._population_generation = 0
        self._early_stop = False
        self._closed = False
        self._reconciled = False

    def reconcile_startup(self, query=systemctl_active):
        """Adopt resident units without starting them, off-thread and FIFO.

        Query failures retain conservative cleanup ownership. The injected query
        must honour its timeout, just like the lifecycle command runner.
        """
        with self._lock:
            if self._closed or self._reconciled:
                return
            self._worker.submit(lambda: self._adopt(query))
            self._reconciled = True

    def _adopt(self, query):
        for engine in self._states:
            with self._lock:
                if self._closed:
                    return
            error = None
            try:
                active = query(engine, self.command_timeout)
                if not isinstance(active, bool):
                    raise RuntimeError(
                        'activity query returned no definitive state')
            except Exception as exc:
                active, error = True, f'{engine}: activity query: {exc}'
            with self._lock:
                if self._closed:
                    return
                state = self._states[engine]
                state.running = state.running or active
                if error:
                    state.error = error
                if state.running and not state.users:
                    state.last_release = self.clock()
                    old = self._idle_timers.pop(engine, None)
                    if old:
                        old.cancel()
                    try:
                        self._idle_timers[engine] = self.call_later(
                            self.idle_timeout, self.sweep)
                    except Exception as exc:
                        state.error = f'{engine}: idle scheduler: {exc}'
                self.sweep()

    def state(self, engine):
        with self._lock:
            return replace(self._states[engine])

    def acquire(self, engine):
        with self._lock:
            if self._closed:
                raise RuntimeError('engine manager closed')
            state = self._states[engine]
            lease = EngineLease(self, engine)
            self._leases.add(lease)
            state.users += 1
            state.generation += 1
            timer = self._idle_timers.pop(engine, None)
            if timer:
                timer.cancel()
            # Queue while holding this short lock to preserve acquisition/stop
            # order.
            # The scheduler must not run this callback inline.
            try:
                self._worker.submit(lambda: self._start(lease))
            except Exception as exc:
                self._complete(lease, RuntimeError(f'{engine}: {exc}'))
                lease.release()
            return lease

    def _complete(self, lease, error=None):
        with self._lock:
            if lease._released or lease.ready.done():
                return
            if error is None:
                lease.ready.set_result(None)
            else:
                self._states[lease.engine].error = str(error)
                lease.ready.set_exception(error)

    def _start(self, lease):
        engine = lease.engine
        with self._lock:
            if lease._released:
                return
            state = self._states[engine]
            start = not state.running or state.error is not None
            if start:
                state.command = 'start'
                state.error = None
        if start:
            try:
                self.runner(engine, 'start', self.command_timeout)
            except Exception as exc:
                with self._lock:
                    state.command = None
                    # A timed-out start may still have activated the service.
                    # Retain cleanup responsibility and force a start on retry.
                    state.running = True
                    state.error = f'{engine}: {exc}'
                self._complete(lease, RuntimeError(f'{engine}: {exc}'))
                self.sweep()
                return
            with self._lock:
                state.running = True
                state.command = None
        with self._lock:
            if lease._released:
                self.sweep()
                return
        try:
            self._readers.submit(lambda: self._await_ready(lease))
        except Exception as exc:
            self._complete(lease, RuntimeError(f'{engine}: {exc}'))

    def _await_ready(self, lease):
        with self._lock:
            if lease._released:
                return
        try:
            if not self.readiness(lease.engine, self.readiness_timeout):
                raise TimeoutError('readiness timeout')
        except Exception as exc:
            self._complete(lease, RuntimeError(f'{lease.engine}: {exc}'))
        else:
            self._complete(lease)

    def _release(self, lease):
        with self._lock:
            if lease._released:
                return
            lease._released = True
            self._leases.discard(lease)
            if lease._timer:
                lease._timer.cancel()
            lease.ready.cancel()
            state = self._states[lease.engine]
            state.users -= 1
            state.last_release = self.clock()
            if state.users == 0 and not self._closed:
                old = self._idle_timers.pop(lease.engine, None)
                if old:
                    old.cancel()
                try:
                    self._idle_timers[lease.engine] = self.call_later(
                        self.idle_timeout, self.sweep)
                except Exception as exc:
                    # Release must not mask a capture/playback exception.
                    # A later
                    # maintenance sweep can still retire this engine.
                    state.error = f'{lease.engine}: idle scheduler: {exc}'
                self.sweep()

    def warm(self, engines=None, *, timeout=60):
        """Temporary leases released on readiness/error or wall-clock timeout.

        Legacy launchers may warm both. Implicit dictation/speech should acquire
        only their backend. Do not wait for these leases on the controller
        thread.
        """
        engines = tuple(self._states if engines is None else engines)
        if timeout <= 0 or any(
            engine not in self._states for engine in engines
        ):
            raise ValueError('invalid warm request')
        leases = []
        try:
            for engine in engines:
                lease = self.acquire(engine)
                leases.append(lease)
                def expire(lease=lease):
                    self._complete(lease, TimeoutError(
                        f'{lease.engine}: warm timeout'))
                    lease.release()
                with self._lock:
                    lease.ready.add_done_callback(
                        lambda _, lease=lease: lease.release())
                    if not lease._released:
                        lease._timer = self.call_later(timeout, expire)
        except Exception:
            for lease in leases:
                lease.release()
            raise
        return tuple(leases)

    def set_population(self, sessions, pending_admissions=0):
        """Publish counts on every registration/admission transition, not from
        status.

        Call before starting admission/warm work. A transition from some
        sessions to none permits early stop; initial zero does not. Pending
        admissions fence
        ALL stops. New population revisions invalidate commands not yet begun.
        """
        if sessions < 0 or pending_admissions < 0:
            raise ValueError('negative population')
        with self._lock:
            if self._sessions > 0 and sessions == 0:
                self._early_stop = True
            if sessions > 0:
                self._early_stop = False
            self._sessions, self._admissions = sessions, pending_admissions
            self._population_generation += 1
            self.sweep()

    def _eligible(self, state):
        return (
            not self._closed
            and state.users == 0
            and state.running
            and not self._admissions
            and (
                self._early_stop
                or self.clock() - state.last_release >= self.idle_timeout
            )
        )

    def sweep(self):
        """Nonblocking idle check, also safe for the controller's maintenance
        timer.
        """
        with self._lock:
            for engine, state in self._states.items():
                if not self._eligible(state):
                    continue
                ticket = (engine, state.generation, self._population_generation)
                if ticket in self._queued_stops:
                    continue
                self._queued_stops.add(ticket)
                try:
                    self._worker.submit(
                        lambda ticket=ticket: self._stop(ticket))
                except Exception as exc:
                    self._queued_stops.discard(ticket)
                    state.error = f'{engine}: {exc}'

    def _stop(self, ticket):
        engine, generation, population = ticket
        with self._lock:
            self._queued_stops.discard(ticket)
            state = self._states[engine]
            if (
                state.generation != generation
                or self._population_generation != population
                or not self._eligible(state)
            ):
                return
            # Commit stop before dropping lock. Any later acquire queues a start
            # behind this command and cannot become ready until that restart
            # ends.
            state.running = False
            state.command = 'stop'
        try:
            self.runner(engine, 'stop', self.command_timeout)
        except Exception as exc:
            with self._lock:
                # Failure may have stopped the unit partially. Force the next
                # acquisition to issue start, while permitting stop retries.
                state.running = True
                state.error = f'{engine}: {exc}'
        else:
            with self._lock:
                state.error = None
        finally:
            with self._lock:
                state.command = None

    def close(self):
        """Cancel leases/timers and join owned bounded workers; no implicit
        unit stop.
        """
        with self._lock:
            self._closed = True
            for lease in list(self._leases):
                lease.release()
            for timer in self._idle_timers.values():
                timer.cancel()
            self._idle_timers.clear()
        if self._own_worker:
            self._worker.shutdown(wait=True, cancel_futures=True)
        if self._own_readers:
            self._readers.shutdown(wait=True, cancel_futures=True)
