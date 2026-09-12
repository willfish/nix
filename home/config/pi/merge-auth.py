#!/usr/bin/env python3
"""Seed missing Pi OAuth providers and drop leftover API-key entries."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


def load_object(path: Path, missing: dict) -> dict:
    if not path.exists():
        return dict(missing)
    payload = json.loads(path.read_text() or "{}")
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must be a JSON object")
    return payload


def read_secret(path: Path) -> str:
    return path.read_text().strip()


def oauth_entry(refresh: str, account_id: str) -> dict:
    return {
        "type": "oauth",
        "refresh": refresh,
        "accountId": account_id,
        "access": "",
        "expires": 0,
    }


def merge_auth(
    auth: dict,
    *,
    drop: list[str],
    oauth_provider: str | None,
    refresh: str | None,
    account_id: str | None,
) -> tuple[dict, bool]:
    merged = dict(auth)
    changed = False
    for name in drop:
        if name in merged:
            del merged[name]
            changed = True
    if oauth_provider and refresh and account_id:
        current = merged.get(oauth_provider)
        replace = (
            not isinstance(current, dict) or current.get("type") != "oauth"
        )
        if replace:
            merged[oauth_provider] = oauth_entry(refresh, account_id)
            changed = True
    return merged, changed


def apply(
    auth_path: Path,
    *,
    drop: list[str],
    oauth_provider: str | None,
    refresh_path: Path | None,
    account_path: Path | None,
) -> bool:
    refresh = None
    account_id = None
    have_files = (
        refresh_path
        and account_path
        and refresh_path.exists()
        and account_path.exists()
    )
    if have_files:
        refresh = read_secret(refresh_path)
        account_id = read_secret(account_path)
        if not refresh or not account_id:
            refresh = None
            account_id = None
    auth = load_object(auth_path, {})
    merged, changed = merge_auth(
        auth,
        drop=drop,
        oauth_provider=oauth_provider,
        refresh=refresh,
        account_id=account_id,
    )
    if not changed:
        return False
    auth_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = auth_path.with_name(auth_path.name + ".tmp")
    temporary.write_text(json.dumps(merged, indent=2) + "\n")
    temporary.chmod(0o600)
    os.replace(temporary, auth_path)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("auth_json", type=Path)
    parser.add_argument("--drop", action="append", default=[])
    parser.add_argument("--oauth-provider")
    parser.add_argument("--refresh-file", type=Path)
    parser.add_argument("--account-file", type=Path)
    args = parser.parse_args()
    if args.oauth_provider and not (args.refresh_file and args.account_file):
        parser.error(
            "--oauth-provider requires --refresh-file and --account-file"
        )
    apply(
        args.auth_json,
        drop=args.drop,
        oauth_provider=args.oauth_provider,
        refresh_path=args.refresh_file,
        account_path=args.account_file,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
