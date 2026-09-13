"""Install a private Hermes declaration without resetting runtime history."""

import argparse
import base64
import fcntl
import json
import os
from pathlib import Path
import tempfile

# Scheduler bookkeeping is not declarative configuration.
RUNTIME = {
    "last_run_at",
    "next_run_at",
    "last_status",
    "last_error",
    "last_delivery_error",
    "last_dispatch",
    "failure_streak",
    "fire_claim",
}


def atomic_write(path, data, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink():
        raise ValueError("Refusing to overwrite a symlink")
    if path.exists() and path.read_bytes() == data:
        os.chmod(path, mode)
        return
    fd, temporary = tempfile.mkstemp(
        dir=path.parent, prefix=".hermes-managed-"
    )
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def safe_path(root, relative):
    path = Path(relative)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise ValueError("Invalid declaration path")
    result = root / path
    if any(
        p.is_symlink() for p in [result, *result.parents] if p != root.parent
    ):
        raise ValueError("Declaration traverses a symlink")
    return result


def reconcile_jobs(declared, existing):
    old = {job["id"]: job for job in existing}
    result = []
    ids = set()
    for spec in declared:
        if spec["id"] in ids:
            raise ValueError("Duplicate declared job ID")
        ids.add(spec["id"])
        job = dict(spec)
        previous = old.get(spec["id"], {})
        # Preserve bookkeeping only for the same schedule. An intentional change
        # gets a fresh next-run computation from the pinned Hermes scheduler.
        if previous.get("schedule") == spec.get("schedule"):
            job.update({k: v for k, v in previous.items() if k in RUNTIME})
            if isinstance(job.get("repeat"), dict):
                job["repeat"] = dict(job["repeat"])
                job["repeat"]["completed"] = previous.get("repeat", {}).get(
                    "completed", 0
                )
            if previous.get("state") == "completed":
                job.update(state="completed", enabled=False, next_run_at=None)
        result.append(job)
    return result


def _with_telegram_qwen_route(root, path, data, mode):
    if path != root / "config.yaml":
        return path, data, mode
    import yaml
    from hermes_telegram_route import merge_routes

    loaded = yaml.safe_load(data)
    if loaded is not None and not isinstance(loaded, dict):
        return path, data, mode
    config, _changed = merge_routes({} if loaded is None else loaded)
    return (
        path,
        yaml.safe_dump(
            config,
            sort_keys=False,
            default_flow_style=False,
            allow_unicode=True,
        ).encode(),
        mode,
    )


def apply(root, declaration, qwen_overlay=None, key_file=None):
    root = Path(root).resolve()
    if declaration.get("version") != 1:
        raise ValueError("Unsupported Hermes declaration")
    # Validate paths and payloads before applying declared files.
    # Never echo the declaration or errors containing its values.
    items = declaration["files"] + [
        item
        for item in declaration.get("seed_files", [])
        if not safe_path(root, item["path"]).exists()
    ]
    for item in items:
        relative = Path(item["path"])
        if not relative.parts:
            raise ValueError("Empty declaration path")
        if str(relative) not in {
            "config.yaml",
            "SOUL.md",
            "AGENTS.md",
            "auth.json",
            "profiles/qwen/config.yaml",
            "profiles/qwen/auth.json",
        } and relative.parts[0] not in {
            "skills",
            "scripts",
            "hooks",
            "plugins",
            "assets",
        }:
            raise ValueError("Declaration cannot own runtime state")
    files = [
        (
            safe_path(root, item["path"]),
            base64.b64decode(item["content"], validate=True),
            0o700 if item.get("executable", False) else 0o600,
        )
        for item in items
    ]
    if len({p for p, _, _ in files}) != len(files):
        raise ValueError("Duplicate declaration path")
    reconcile_jobs(declaration["jobs"], [])
    new_key = None
    if qwen_overlay is not None:
        import secrets
        import yaml
        from hermes_profile import merge_profile

        target = root / "profiles/qwen/config.yaml"
        profile = next(
            (
                yaml.safe_load(data)
                for path, data, _ in files
                if path == target
            ),
            None,
        )
        if not isinstance(profile, dict) or key_file is None:
            raise ValueError("Qwen configuration or key path missing")
        key_file = Path(key_file)
        if key_file.exists():
            key = key_file.read_text().strip()
            if not key:
                raise ValueError("Local model key is empty")
        else:
            key = profile.get("model", {}).get("api_key") or secrets.token_hex(
                32
            )
            new_key = (key_file, key.encode() + b"\n", 0o600)
        profile = merge_profile(profile, qwen_overlay)
        profile.setdefault("model", {})["api_key"] = key
        for provider in profile.get("custom_providers", []):
            if provider.get("name") == "qwen-local":
                provider["api_key"] = key
        files = [
            (
                path,
                (
                    yaml.safe_dump(profile, sort_keys=False).encode()
                    if path == target
                    else data
                ),
                mode,
            )
            for path, data, mode in files
        ]
    files = [
        _with_telegram_qwen_route(root, path, data, mode)
        for path, data, mode in files
    ]
    jobs_path = safe_path(root, "cron/jobs.json")
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(root, 0o700)
    lock_path = safe_path(root, "cron/.jobs.lock")
    jobs_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with lock_path.open("a") as lock:
        import time

        deadline = time.monotonic() + 30
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "Hermes scheduler is busy; activation aborted"
                    ) from None
                time.sleep(0.1)
        existing = (
            json.loads(jobs_path.read_text())
            if jobs_path.exists()
            else {"jobs": []}
        )
        index = safe_path(root, ".home-manager-files.json")
        previous_paths = (
            json.loads(index.read_text()) if index.exists() else []
        )
        declared_paths = sorted(item["path"] for item in declaration["files"])
        allowed_roots = {"skills", "scripts", "hooks", "plugins", "assets"}
        allowed_files = {
            "config.yaml",
            "SOUL.md",
            "AGENTS.md",
            "profiles/qwen/config.yaml",
        }
        for path in previous_paths:
            parts = Path(path).parts
            if path not in allowed_files and (
                not parts or parts[0] not in allowed_roots
            ):
                raise ValueError("Managed-file index contains a runtime path")
        stale = [
            safe_path(root, path)
            for path in previous_paths
            if path not in declared_paths
        ]
        backups = safe_path(root, "backups/home-manager")
        markers = [
            safe_path(root, ".managed"),
            safe_path(root, "profiles/qwen/.managed"),
        ]
        jobs = reconcile_jobs(declaration["jobs"], existing.get("jobs", []))
        # Back up exactly what this activation will replace, once per content.
        import hashlib

        for path, data, mode in [
            *([] if new_key is None else [new_key]),
            *files,
            (
                jobs_path,
                json.dumps({**existing, "jobs": jobs}, indent=2).encode()
                + b"\n",
                0o600,
            ),
        ]:
            if path.exists() and path.read_bytes() != data:
                previous = path.read_bytes()
                digest = hashlib.sha256(previous).hexdigest()
                backup = backups / digest
                atomic_write(backup, previous)
            atomic_write(path, data, mode)
        for path in stale:
            if path.is_file():
                previous = path.read_bytes()
                atomic_write(
                    backups / hashlib.sha256(previous).hexdigest(),
                    previous,
                )
                path.unlink()
        atomic_write(index, json.dumps(declared_paths).encode() + b"\n")
        for marker in markers:
            atomic_write(marker, b"home-manager\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("declaration", type=Path)
    parser.add_argument("home", type=Path)
    parser.add_argument("--qwen-overlay", type=Path)
    parser.add_argument("--key-file", type=Path)
    args = parser.parse_args()
    try:
        apply(
            args.home,
            json.loads(args.declaration.read_text()),
            (
                json.loads(args.qwen_overlay.read_text())
                if args.qwen_overlay
                else None
            ),
            args.key_file,
        )
    except Exception as error:
        raise SystemExit(
            f"Hermes activation failed ({type(error).__name__}); "
            "secret values have not been logged"
        ) from None
