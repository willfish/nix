"""Check the installed public Pi profile without invoking a model or server."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

home = Path.home()
env = dict(os.environ, PI_TELEMETRY="0", PI_OFFLINE="1")
with tempfile.TemporaryFile(mode="w+") as errors:
    process = subprocess.Popen(
        [str(home / ".local/bin/pi"), "--mode", "rpc", "--offline",
         "--no-session"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
        env=env,
    )
    try:
        request = {"id": "public-startup", "type": "get_state"}
        process.stdin.write((json.dumps(request) + "\n").encode())
        process.stdin.flush()
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 30
        response = None
        pending = b""
        while time.monotonic() < deadline and process.poll() is None:
            if not selector.select(1):
                continue
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                break
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                item = json.loads(line)
                if item.get("id") == "public-startup":
                    response = item
            if response is not None:
                break
        assert response is not None, "Pi did not answer get_state"
        assert response.get("success"), response
        errors.flush()
        errors.seek(0)
        stderr = errors.read()
        assert "failed to load" not in stderr.lower(), stderr
        print("Public Pi starts and answers RPC state without credentials.")
        if stderr:
            print(stderr)
    finally:
        process.terminate()
        process.wait(timeout=10)
