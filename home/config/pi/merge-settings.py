#!/usr/bin/env python3
"""Fill missing keys in a writable Pi settings.json from declared defaults.

Also retarget a leftover grok-4.6 defaultModel to grok-4.7 so existing
profiles pick up the current Grok default without clobbering other keys.

The previous home default turned observational memory on. Flip that exact
leftover once, then leave /settings in charge of enabledByDefault.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


OM_KEY = "observational-memory-jev"
OM_DEFAULT_OFF_MARKER = ".om-default-off-migrated"


def merge_missing(defaults: dict, settings: dict) -> tuple[dict, bool]:
    merged = dict(settings)
    changed = False
    for key, value in defaults.items():
        if key not in merged:
            merged[key] = value
            changed = True
    if (
        merged.get("defaultModel") == "grok-4.6"
        and defaults.get("defaultModel") == "grok-4.7"
    ):
        merged["defaultModel"] = "grok-4.7"
        changed = True
    return merged, changed


def retarget_om_default_off(
    defaults: dict, settings: dict, settings_path: Path
) -> tuple[bool, bool]:
    """Turn off the previous home-managed om default, once per profile.

    Returns (settings_changed, record_marker). The marker is written by the
    caller after the settings file is durable, so a failed write can retry.
    """
    declared = defaults.get(OM_KEY)
    if not isinstance(declared, dict):
        return False, False
    if declared.get("enabledByDefault") is not False:
        return False, False
    marker = settings_path.with_name(OM_DEFAULT_OFF_MARKER)
    if marker.exists():
        return False, False
    current = settings.get(OM_KEY)
    changed = False
    if isinstance(current, dict) and current.get("enabledByDefault") is True:
        current["enabledByDefault"] = False
        changed = True
    return changed, True


def write_om_default_off_marker(settings_path: Path) -> None:
    marker = settings_path.with_name(OM_DEFAULT_OFF_MARKER)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text("observational memory home default is off\n")
    marker.chmod(0o600)


def apply(defaults_path: Path, settings_path: Path) -> bool:
    defaults = json.loads(defaults_path.read_text())
    if not isinstance(defaults, dict):
        raise ValueError("defaults must be a JSON object")
    if settings_path.exists():
        settings = json.loads(settings_path.read_text() or "{}")
        if not isinstance(settings, dict):
            raise ValueError("settings must be a JSON object")
    else:
        settings = {}
    merged, changed = merge_missing(defaults, settings)
    om_changed, record_marker = retarget_om_default_off(
        defaults, merged, settings_path
    )
    changed = om_changed or changed
    if changed:
        settings_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = settings_path.with_name(settings_path.name + ".tmp")
        temporary.write_text(json.dumps(merged, indent=2) + "\n")
        temporary.chmod(0o600)
        os.replace(temporary, settings_path)
    if record_marker:
        write_om_default_off_marker(settings_path)
    return changed


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <defaults.json> <settings.json>",
            file=sys.stderr,
        )
        return 2
    apply(Path(sys.argv[1]), Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
