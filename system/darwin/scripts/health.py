"""Local-only service health and bounded retention of explicitly owned logs."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile


def command(args):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=5)
        return p.returncode, p.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 124, ""


def service_status(label, kind, run=command):
    code, output = run(["/bin/launchctl", "print", "system/" + label])
    # launchctl also prints environment values. Never persist its raw output.
    state = re.search(r"^\s*state = ([a-z ]+)\s*$", output, re.M)
    exit_code = re.search(r"^\s*last exit code = (-?\d+)", output, re.M)
    state = state.group(1).strip() if state else "unknown"
    last_exit = int(exit_code.group(1)) if exit_code else None
    healthy = code == 0 and (
        kind == "socket"
        or (
            state == "running"
            if kind == "running"
            else state != "running" and last_exit == 0
        )
    )
    return {"healthy": healthy, "state": state, "lastExit": last_exit}


def snapshot(config, run=command):
    services = {
        s["label"]: service_status(s["label"], s["kind"], run)
        for s in config["services"]
    }
    ready = run(config["readiness"])[0] == 0
    vm_code, vm = run(["/usr/bin/vm_stat"])
    memory = {
        key: int(value)
        for key, value in re.findall(
            r"^(Pages (?:free|active|wired down|occupied by compressor)):"
            r"\s*(\d+)",
            vm,
            re.M,
        )
    }
    swap_code, swap = run(["/usr/sbin/sysctl", "-n", "vm.swapusage"])
    used = re.search(r"used = ([\d.]+[KMGT])", swap)
    return {
        "time": datetime.now(timezone.utc).isoformat(),
        "healthy": ready and all(s["healthy"] for s in services.values()),
        "ready": ready,
        "services": services,
        "memoryPages": memory,
        "swapUsed": used.group(1) if used else None,
        "metricsAvailable": vm_code == 0 and swap_code == 0,
    }


def atomic_json(path, data):
    fd, tmp = tempfile.mkstemp(prefix=".health.", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(data, f)
        os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


def rotate(path, limit):
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return False
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise ValueError("Refusing a non-regular log")
        if st.st_size <= limit:
            return False
        # Copytruncate keeps launchd's open stdout/stderr descriptors valid.
        # Truncation can lose concurrent writes; no service is signalled.
        tail = os.pread(fd, limit, st.st_size - limit)
        previous = path.with_name(path.name + ".1")
        if previous.exists():
            previous.replace(path.with_name(path.name + ".2"))
        out, tmp = tempfile.mkstemp(prefix=".log.", dir=path.parent)
        try:
            with os.fdopen(out, "wb") as f:
                f.write(tail)
            os.replace(tmp, previous)
        finally:
            Path(tmp).unlink(missing_ok=True)
        os.ftruncate(fd, 0)
        return True
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    result = snapshot(config)
    if args.record:
        if os.geteuid() != 0:
            parser.error("recording and log retention require root")
        result["rotated"] = sum(
            rotate(Path(p), config["logLimitBytes"]) for p in config["logs"]
        )
        atomic_json(Path(config["statusFile"]), result)
    print(json.dumps(result))
    return 0 if result["healthy"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
