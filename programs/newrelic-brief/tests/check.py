#!/usr/bin/env python3
"""Manual contract checks. Pass a binary built with testMode = true."""

import copy
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import termios
import time

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="newrelic-brief-check-") as scratch:
    base = Path(scratch)
    data = base / "data"
    config = base / "config" / "newrelic-brief"
    config.mkdir(parents=True)
    fixture = base / "fixture.json"
    calls = base / "calls.jsonl"
    pi = base / "pi"
    pi.write_text("""#!/usr/bin/env python3
import json, os, sys
prompt = sys.stdin.read()
assert os.environ['NEW_RELIC_READ_ONLY'] == '1'
assert os.environ['PI_MCP_CONFIG_MODE'] == 'exclusive'
assert os.environ['PI_AGENT_BUS_ENABLED'] == '0'
assert 'MCP_DIRECT_TOOLS' not in os.environ
assert 'CAPTURE_PROMPTS' not in os.environ
args = sys.argv[1:]
flags = ['--print', '--no-session', '--no-extensions', '--no-skills',
         '--no-context-files', '--no-approve']
for flag in flags:
    assert flag in args, flag
assert args[args.index('--exclude-tools') + 1] == 'mcp,codemode,tool_search'
expected = {'get_account_details', 'list_apm_applications',
            'list_open_incidents', 'run_nrql_query', 'run_nerdgraph_query',
            'get_entity_details', 'list_deployments_rest'}
assert set(args[args.index('--tools') + 1].split(',')) == expected
assert 'Current Europe/London time:' in prompt
with open(os.environ['BRIEF_CALLS'], 'a') as out:
    record = {'args': args, 'has_history': 'issue:example' in prompt}
    out.write(json.dumps(record) + '\\n')
fixture = json.load(open(os.environ['BRIEF_FIXTURE']))
if fixture.pop('_fail', False):
    sys.exit(1)
print('<<<NEWRELIC-BRIEF')
print(json.dumps(fixture, separators=(',', ':')))
print('NEWRELIC-BRIEF>>>')
""")
    pi.chmod(0o700)
    settings = {
        "provider": "fixture",
        "model": "fixture",
        "thinking": "off",
        "timeout_seconds": 30,
        "pi_command": str(pi),
        "pi_extension": "fixture",
        "mcp_config": "fixture",
        "prompt_file": str(config / "prompt.md"),
    }
    (config / "prompt.md").write_text("Offline reporting fixture.")
    # Home Manager deploys configuration through symlinks.
    settings_path = base / "settings.json"
    settings_path.write_text(json.dumps(settings))
    (config / "config.json").symlink_to(settings_path)
    env = dict(
        os.environ,
        XDG_DATA_HOME=str(data),
        XDG_CONFIG_HOME=str(base / "config"),
        NEWRELIC_BRIEF_NOW="2026-10-19T09:00:00+01:00",
        BRIEF_FIXTURE=str(fixture),
        BRIEF_CALLS=str(calls),
        MCP_DIRECT_TOOLS="unwanted",
        CAPTURE_PROMPTS="1",
        NO_COLOR="1",
    )
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    root = data / "newrelic-brief"

    def run(command="run", ok=True):
        result = subprocess.run(
            [binary, command],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        assert (result.returncode == 0) == ok, (
            result.returncode,
            result.stderr,
        )
        return result

    def reports():
        return sorted(root.glob("*/report-*.json"))

    finding = {
        "key": "issue:example",
        "state": "open:high",
        "severity": "high",
        "title": "Example latency issue",
        "detail": "Synthetic data: 150 ms over 600 requests in the last hour.",
        "evidence": "Synthetic New Relic issue example; not live telemetry.",
    }
    report = {
        "status": "ok",
        "coverage": "Synthetic account, issues and APM over the last hour.",
        "findings": [finding],
    }

    def fixture_report(value):
        fixture.write_text(json.dumps(value))

    fixture_report(report)
    run()
    assert len(reports()) == 1
    first = reports()[0]
    assert json.loads(first.read_text())["shown_at"] == 0
    assert (root.stat().st_mode & 0o777) == 0o700
    assert (first.stat().st_mode & 0o777) == 0o600
    run()
    assert len(reports()) == 1, (
        "pending findings must deduplicate without being lost"
    )
    changed_wording = copy.deepcopy(report)
    changed_wording["findings"][0]["detail"] = (
        "Synthetic wording changed; state did not."
    )
    fixture_report(changed_wording)
    run()
    assert len(reports()) == 1
    assert json.loads(calls.read_text().splitlines()[-1])["has_history"]
    print(
        "PASS: private state, Pi isolation, pending and wording deduplication"
    )

    # A real PTY exercises pager startup, seen markers, q and mode restoration.
    master, slave = pty.openpty()
    fcntl.ioctl(
        slave,
        termios.TIOCSWINSZ,
        __import__("struct").pack("HHHH", 24, 48, 0, 0),
    )
    before = termios.tcgetattr(slave)

    def terminal_session():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    child = subprocess.Popen(
        [binary, "_view"],
        env=dict(env, TERM="xterm-256color"),
        stdin=slave,
        stdout=slave,
        stderr=slave,
        preexec_fn=terminal_session,
    )
    screen = b""
    deadline = time.monotonic() + 10
    while b"NEW RELIC" not in screen and time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.2)
        if ready:
            screen += os.read(master, 65536)
    assert b"NEW RELIC" in screen, screen
    os.write(master, b"q")
    assert child.wait(timeout=10) == 0
    assert termios.tcgetattr(slave) == before, (
        "pager must restore terminal modes"
    )
    os.close(master)
    os.close(slave)
    assert json.loads(first.read_text())["shown_at"] > 0
    assert "EARLIER" in run("print").stdout
    launches = base / "launches"
    systemd_run = base / "systemd-run"
    systemd_run.write_text(
        '#!/bin/sh\nprintf "%s\\n" "$*" >> "$BRIEF_LAUNCHES"\n'
    )
    systemd_run.chmod(0o700)
    env.update(
        PATH=str(base) + os.pathsep + env["PATH"],
        WAYLAND_DISPLAY="fixture-display",
        BRIEF_LAUNCHES=str(launches),
    )
    run()
    assert len(reports()) == 1
    assert not launches.exists(), "unchanged findings must not reopen"
    print("PASS: narrow PTY, no colour, q, mode restoration and quiet repeat")

    # Compare with the latest state, not every old state.
    recovery = copy.deepcopy(report)
    recovery["findings"][0].update(
        state="closed", severity="recovery", title="Synthetic recovery"
    )
    fixture_report(recovery)
    run()
    assert len(reports()) == 2
    assert "--title=New Relic" in launches.read_text(), (
        "a new recovery must open the scratchpad"
    )
    fixture_report(report)
    run()
    assert len(reports()) == 3, (
        "reopening must not match the earlier open state"
    )
    assert len(launches.read_text().splitlines()) == 2
    print("PASS: recovery and reopening trigger scratchpad launches")

    empty = {
        "status": "ok",
        "coverage": "Synthetic checks completed.",
        "findings": [],
    }
    fixture_report(empty)
    run()
    assert len(reports()) == 3
    fixture_report({"_fail": True})
    run(ok=False)
    assert len(reports()) == 3
    assert (
        json.loads((root / "last-run.json").read_text())["status"] == "failed"
    )
    fixture_report(
        {
            "status": "unavailable",
            "coverage": "Synthetic API unavailable.",
            "findings": [],
        }
    )
    run(ok=False)
    malicious = copy.deepcopy(report)
    malicious["findings"][0]["detail"] = "Unsafe terminal escape: \x1b[2J"
    fixture_report(malicious)
    run(ok=False)
    assert len(reports()) == 3
    verbose = copy.deepcopy(report)
    verbose["findings"][0]["detail"] = "x" * 601
    fixture_report(verbose)
    run(ok=False)
    assert len(reports()) == 3, "verbose output must not reach the viewer"
    partial = {
        "status": "partial",
        "coverage": "Synthetic issue source unavailable.",
        "findings": [],
    }
    fixture_report(partial)
    run()
    assert (
        json.loads((root / "last-run.json").read_text())["status"] == "partial"
    )
    print("PASS: empty/failed/partial checks, control and verbosity limits")

    count = len(calls.read_text().splitlines())
    env["NEWRELIC_BRIEF_NOW"] = "2026-10-23T09:00:00+01:00"
    expired = root / "2026-10-18"
    expired.mkdir()
    (expired / "report-1.json").write_text("expired")
    (expired / "run-1.log").write_text("expired")
    kept = root / "2026-10-19"
    run("prune")
    assert not expired.exists()
    assert kept.exists(), (
        "today plus the preceding four calendar days must be retained"
    )
    env["NEWRELIC_BRIEF_NOW"] = "2026-10-24T09:00:00+01:00"
    run("prune")
    assert not kept.exists()
    fixture_report(report)
    run()
    assert len(calls.read_text().splitlines()) == count
    assert (
        json.loads((root / "last-run.json").read_text())["status"] == "weekend"
    )
    print("PASS: weekdays only; five-calendar-day report and log rotation")

print("All newrelic-brief contract checks passed.")
