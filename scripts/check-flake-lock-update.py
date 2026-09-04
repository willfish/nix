#!/usr/bin/env python3
"""Fail closed unless a lock update only advances approved GitHub sources.

Run trusted base-branch code against both lock files; never evaluate PR inputs.
Graph or source metadata changes always need manual review. Every node is
checked, including nodes that are not currently reachable from the root.
"""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import sys


class ManualReview(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ManualReview(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def nonempty_string(value):
    return (isinstance(value, str) and bool(value)
            and not any(ord(c) < 32 for c in value))


def validate_source(source, label, *, locked):
    require(isinstance(source, dict), f"{label}: expected a source object")
    required = {
        "github": ("owner", "repo"), "gitlab": ("owner", "repo"),
        "sourcehut": ("owner", "repo"), "git": ("url",),
        "mercurial": ("url",), "tarball": ("url",), "file": ("url",),
        "path": ("path",), "indirect": ("id",),
    }
    source_type = source.get("type")
    require(isinstance(source_type, str) and source_type in required,
            f"{label}: unsupported source type")
    require(not locked or source_type != "indirect",
            f"{label}: unresolved source")
    for field in required[source_type]:
        require(nonempty_string(source.get(field)),
                f"{label}: missing or invalid {field}")
    require(all(type(value) in (str, int, bool) for value in source.values()),
            f"{label}: unsupported source attributes")
    for field in ("rev", "ref", "host", "dir"):
        if field in source:
            require(isinstance(source[field], str), f"{label}: invalid {field}")
    if source_type == "github":
        require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*", source["owner"])
                and re.fullmatch(r"[A-Za-z0-9_.-]+", source["repo"])
                and source["repo"] not in (".", ".."),
                f"{label}: invalid GitHub repository")
    if locked:
        digest = source.get("narHash", "")
        require(isinstance(digest, str) and digest.startswith("sha256-"),
                f"{label}: missing or unsupported content hash")
        try:
            require(len(base64.b64decode(digest[7:], validate=True)) == 32,
                    f"{label}: invalid content hash")
        except ValueError as error:
            raise ManualReview(f"{label}: invalid content hash") from error
        if source_type == "github":
            require(isinstance(source.get("rev"), str)
                    and re.fullmatch(r"[0-9a-f]{40}", source["rev"]),
                    f"{label}: missing or invalid GitHub revision")
    for field in ("lastModified", "revCount"):
        if field in source:
            require(type(source[field]) is int and source[field] >= 0,
                    f"{label}: invalid {field}")


def validate_lock(lock):
    require(isinstance(lock, dict)
            and set(lock) == {"nodes", "root", "version"},
            "unsupported lock-file structure")
    require(type(lock["version"]) is int and lock["version"] == 7,
            "unsupported lock-file version")
    nodes, root = lock["nodes"], lock["root"]
    require(isinstance(nodes, dict) and nodes
            and nonempty_string(root) and root in nodes,
            "missing or invalid root node")
    for name, node in nodes.items():
        require(nonempty_string(name) and isinstance(node, dict),
                "invalid node")
        allowed = ({"inputs"} if name == root
                   else {"inputs", "locked", "original", "flake"})
        require(set(node) <= allowed, f"{name!r}: unsupported node attributes")
        inputs = node.get("inputs", {})
        require(isinstance(inputs, dict), f"{name!r}: invalid inputs")
        if "flake" in node:
            require(type(node["flake"]) is bool,
                    f"{name!r}: invalid flake flag")
            require(node["flake"] or not inputs,
                    f"{name!r}: non-flake has inputs")
        if name != root:
            for field in ("locked", "original"):
                validate_source(node.get(field), f"{name!r}.{field}",
                                locked=field == "locked")
        for input_name, target in inputs.items():
            require(nonempty_string(input_name),
                    f"{name!r}: invalid input name")
            if isinstance(target, str):
                require(target in nodes,
                        f"{name!r}: dangling input reference")
            else:
                require(isinstance(target, list)
                        and all(nonempty_string(part) for part in target),
                        f"{name!r}: unsupported input reference")

    # A follows path starts at the root, and each hop can itself be a follows.
    # Track resolving edges, not nodes: ordinary shared or circular dependencies
    # are legal, but a follows alias that recursively resolves itself is not.
    resolving, resolved = set(), {}

    def resolve(name, input_name):
        edge = (name, input_name)
        require(edge not in resolving, f"{name!r}.{input_name}: follows cycle")
        if edge in resolved:
            return resolved[edge]
        require(input_name in nodes[name].get("inputs", {}),
                f"{name!r}.{input_name}: dangling follows path")
        resolving.add(edge)
        target = nodes[name]["inputs"][input_name]
        if isinstance(target, list):
            destination = root
            for part in target:
                destination = resolve(destination, part)
        else:
            destination = target
        resolving.remove(edge)
        resolved[edge] = destination
        return destination

    for name, node in nodes.items():
        for input_name in node.get("inputs", {}):
            resolve(name, input_name)


def canonical(value):
    # Preserve JSON types when comparing, including true versus 1.
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def approved_changes(base, head, owners):
    validate_lock(base)
    validate_lock(head)
    require(base["root"] == head["root"], "root node changed")
    require(base["nodes"].keys() == head["nodes"].keys(),
            "nodes were added or removed")
    changed = []
    for name, before in base["nodes"].items():
        after = head["nodes"][name]
        metadata = lambda node: {
            key: value for key, value in node.items() if key != "locked"
        }
        require(canonical(metadata(before)) == canonical(metadata(after)),
                f"{name!r}: input graph or source metadata changed")
        old, new = before.get("locked"), after.get("locked")
        if canonical(old) == canonical(new):
            continue
        require(old["type"] == new["type"] == "github",
                f"{name!r}: changed non-GitHub source")
        mutable = {"rev", "narHash", "lastModified"}
        identity = lambda source: {
            key: value for key, value in source.items() if key not in mutable
        }
        require(canonical(identity(old)) == canonical(identity(new)),
                f"{name!r}: locked source changed")
        require(set(new) <= mutable | {"type", "owner", "repo", "host", "dir"},
                f"{name!r}: unsupported GitHub attributes")
        require(new.get("host", "github.com") == "github.com",
                f"{name!r}: unapproved GitHub host")
        require(new["owner"] in owners,
                f"{name!r}: GitHub owner is not auto-mergeable")
        changed.append(name)
    return sorted(changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("head", type=Path)
    args = parser.parse_args()
    owners = set(filter(
        None, os.environ.get("AUTO_MERGE_GITHUB_OWNERS", "").splitlines()
    ))
    try:
        locks = [json.loads(path.read_text(), object_pairs_hook=unique_object)
                 for path in (args.base, args.head)]
        changed = approved_changes(*locks, owners)
    except (ValueError, OSError, RecursionError) as error:
        print(f"Manual review required: {error}.", file=sys.stderr)
        return 1
    print("Auto-merge allowed for changed lock node(s): " + ", ".join(changed)
          if changed else "No changed lock nodes found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
