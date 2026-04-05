#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import urlopen


DEFAULT_HOST = '127.0.0.1'
DEFAULT_CTX_SIZE = '40960'
DEFAULT_MODEL_PORT = 8080
DEFAULT_SHIM_PORT = 8090
READINESS_TIMEOUT_SECONDS = 60.0
POLL_INTERVAL_SECONDS = 0.2


@dataclass(frozen=True)
class RuntimeConfig:
    host: str
    model_port: int
    shim_port: int
    runtime_root: Path
    llm_env: dict[str, str]
    shim_env: dict[str, str]
    claude_env: dict[str, str]


def allocate_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((DEFAULT_HOST, 0))
        sock.listen(1)
        return int(sock.getsockname()[1])


def build_runtime_config(runtime_root: Path | None = None) -> RuntimeConfig:
    host = DEFAULT_HOST
    model_port = allocate_port()
    shim_port = allocate_port()
    while shim_port == model_port:
        shim_port = allocate_port()

    root = runtime_root if runtime_root is not None else Path(tempfile.mkdtemp(prefix='claude-gemma-'))
    root.mkdir(parents=True, exist_ok=True)

    llm_env = {
        'LLM_GEMMA_HOST': host,
        'LLM_GEMMA_PORT': str(model_port),
        'LLM_GEMMA_CTX_SIZE': os.environ.get('LLM_GEMMA_CTX_SIZE', DEFAULT_CTX_SIZE),
    }

    shim_env = {
        'CLAUDE_GEMMA_SHIM_HOST': host,
        'CLAUDE_GEMMA_SHIM_PORT': str(shim_port),
        'LLM_GEMMA_HOST': host,
        'LLM_GEMMA_PORT': str(model_port),
    }

    auth_token = os.environ.get('CLAUDE_GEMMA_AUTH_TOKEN', 'claude-gemma-local')
    claude_env = {
        'ANTHROPIC_AUTH_TOKEN': auth_token,
        'ANTHROPIC_API_KEY': auth_token,
        'ANTHROPIC_BASE_URL': f'http://{host}:{shim_port}',
    }

    return RuntimeConfig(
        host=host,
        model_port=model_port,
        shim_port=shim_port,
        runtime_root=root,
        llm_env=llm_env,
        shim_env=shim_env,
        claude_env=claude_env,
    )


def wait_for_http(url: str, *, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = None
    while time.monotonic() < deadline:
        try:
            with urlopen(url, timeout=2) as response:
                if 200 <= response.status < 500:
                    return
        except HTTPError as exc:
            if 400 <= exc.code < 500:
                return
            last_error = exc
        except Exception as exc:
            last_error = exc
        time.sleep(POLL_INTERVAL_SECONDS)
    raise RuntimeError(f'timed out waiting for {url}: {last_error}')


def terminate_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def start_process(command: list[str], *, env: dict[str, str], log_path: Path) -> subprocess.Popen[bytes]:
    log_file = log_path.open('wb')
    return subprocess.Popen(
        command,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('claude_args', nargs='*')
    return parser


def should_passthrough(args: list[str]) -> bool:
    return any(arg in {'-h', '--help', 'help', '-v', '--version'} for arg in args)


def main(argv: list[str]) -> int:
    claude_args = argv[1:]
    claude_bin = shutil.which('claude')
    llm_bin = shutil.which('llm-gemma')
    shim_bin = shutil.which('claude-gemma-shim')

    if claude_bin is None or llm_bin is None or shim_bin is None:
        print('claude-gemma: required commands were not found in PATH', file=sys.stderr)
        return 1

    if should_passthrough(claude_args):
        return subprocess.call([claude_bin, *claude_args])

    runtime = build_runtime_config()
    processes: list[subprocess.Popen[bytes]] = []

    def cleanup(*_args: object) -> None:
        for process in reversed(processes):
            terminate_process(process)
        shutil.rmtree(runtime.runtime_root, ignore_errors=True)

    previous_sigint = signal.signal(signal.SIGINT, cleanup)
    previous_sigterm = signal.signal(signal.SIGTERM, cleanup)

    try:
        base_env = os.environ.copy()

        llm_log = runtime.runtime_root / 'llm-gemma.log'
        llm_env = base_env | runtime.llm_env
        llm_process = start_process([llm_bin, '--no-webui', '--parallel', '1'], env=llm_env, log_path=llm_log)
        processes.append(llm_process)
        wait_for_http(f'http://{runtime.host}:{runtime.model_port}/', timeout_seconds=READINESS_TIMEOUT_SECONDS)

        shim_log = runtime.runtime_root / 'claude-gemma-shim.log'
        shim_env = base_env | runtime.shim_env
        shim_process = start_process([shim_bin], env=shim_env, log_path=shim_log)
        processes.append(shim_process)
        wait_for_http(f'http://{runtime.host}:{runtime.shim_port}/health', timeout_seconds=READINESS_TIMEOUT_SECONDS)

        claude_env = base_env | runtime.claude_env
        claude_process = subprocess.Popen([claude_bin, *claude_args], env=claude_env)
        return_code = claude_process.wait()
        return return_code
    finally:
        cleanup()
        signal.signal(signal.SIGINT, previous_sigint)
        signal.signal(signal.SIGTERM, previous_sigterm)


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
