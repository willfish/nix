"""Merge a non-secret Home Manager overlay into an existing Hermes profile."""

import argparse
from copy import deepcopy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import tempfile

import yaml


def merge_profile(original, overlay):
    result = deepcopy(original)
    for key, value in overlay.items():
        if key == "custom_providers":
            providers = result.setdefault(key, [])
            for provider in value:
                for index, existing in enumerate(providers):
                    if existing.get("name") == provider["name"]:
                        providers[index] = merge_profile(existing, provider)
                        break
                else:
                    providers.append(deepcopy(provider))
        elif isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge_profile(result[key], value)
        else:
            result[key] = deepcopy(value)
    return result


def apply_profile(profile, overlay, key_file=None):
    profile, overlay = Path(profile), Path(overlay)
    if profile.is_symlink():
        raise ValueError("Refusing to replace a symlinked Hermes config")
    original_text = profile.read_text() if profile.exists() else ""
    original = yaml.safe_load(original_text) or {}
    updated = merge_profile(original, json.loads(overlay.read_text()))
    if key_file and Path(key_file).is_file():
        key = Path(key_file).read_text().strip()
        if not key:
            raise ValueError("Local model API key file is empty")
        updated.setdefault("model", {})["api_key"] = key
        for provider in updated.get("custom_providers", []):
            if provider.get("name") == "qwen-local":
                provider["api_key"] = key
    if updated == original:
        return False
    profile.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if original_text:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        backup = profile.with_name(profile.name + ".before-local-llm-" + stamp)
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write(original_text)
    fd, temporary = tempfile.mkstemp(prefix=".local-llm-", dir=profile.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            yaml.safe_dump(updated, handle, sort_keys=False)
        os.replace(temporary, profile)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile")
    parser.add_argument("overlay")
    parser.add_argument("--key-file")
    args = parser.parse_args()
    if apply_profile(args.profile, args.overlay, key_file=args.key_file):
        print("Updated Qwen profile; previous config backed up if present.")
