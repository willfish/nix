# New Relic brief

A native C companion to Pi. On weekdays at 09:00, 13:00 and 16:00 Europe/London,
Pi gathers read-only New Relic telemetry and returns structured findings. New
findings open a themed, centred Ghostty scratchpad, matching the agenda applet.
Unchanged reports stay quiet. The daily launcher can reopen the notebook.

```sh
newrelic-brief show                         # Open retained reports, without a model call
newrelic-brief print                        # Plain text; does not mark reports shown
systemctl --user start newrelic-brief       # Run a check now; weekends are skipped
journalctl --user -u newrelic-brief.service  # Check collection/delivery failures
```

The viewer uses `less`: `q` closes, arrows or `j`/`k` scroll. It inherits the current
Ghostty theme and supports `NO_COLOR`. Every report shows its collection time,
coverage, severity, evidence and whether it was previously shown. New material
comes first, with urgent findings before lower-priority observations. The theme's
red, amber and positive green/teal mark urgency, warnings and recovery. Labels
stay in the readable terminal foreground; colour is a redundant marker, not the
only signal. Reviewed history uses neutral styling so an old alert does not
compete with a new recovery. Reports are limited to six concise findings.
This is a historical notebook, not a continuously refreshed dashboard.

## Novelty and history

Pi assigns stable finding keys and categorical states. The program compares
those keys, states and severities with their latest retained versions; different
wording or counts alone cannot trigger a popup. Recoveries, escalations and
reopenings can. Semantic classification still depends on the model, so the prompt
requires reuse of previous identities and suppresses minor measurement drift.

State lives under `${XDG_DATA_HOME:-~/.local/share}/newrelic-brief`:

- `YYYY-MM-DD/report-*.json`: validated new findings and their shown timestamps;
- `YYYY-MM-DD/run-*.log`: private Pi stderr, final output and collection result;
- `last-run.json`: last check time, outcome and new-finding count;
- `lock`: serialises collection, retention and viewer state updates.

Only today and the preceding four calendar days of reports/logs are retained.
Pruning runs when the program opens and daily at 00:10 London time, including
weekends without invoking Pi. Directories are private and files use mode 0600.

A report stays pending until its terminal viewer successfully starts the pager.
Missing desktop access, a failed terminal launch or a failed pager does not mark
it shown. A later successful check retries pending findings, even when that check
found no additional changes. Once shown, retained content remains available in
the notebook without reopening automatically. Unavailable or malformed checks
fail visibly in the service journal; partial coverage is recorded separately.

## Configuration and safety

`home/user/newrelic-brief.nix` deploys configuration under
`~/.config/newrelic-brief/` on work-enabled graphical Linux hosts. It uses the same
Pi provider/model as the standup recap, with a ten-minute collection timeout.
`config.json` selects the Pi executable, adapter extension, exclusive MCP config,
prompt, provider, model, thinking and timeout. The maintained reporting prompt is
`home/config/newrelic-brief/prompt.md`.

Pi runs without sessions, context files, discovered extensions, skills, shell or
file tools. Its only declared tools are seven New Relic reads. An exclusive
adapter configuration excludes other servers and filters the New Relic catalogue.
The generic MCP gateway, codemode and tool search are disabled. The installed
New Relic wrapper independently enforces `NEW_RELIC_READ_ONLY=1`, including for
raw GraphQL queries, and reads credentials from sops-nix runtime files. No secret
values belong in configuration, source or the Nix store.

Timers are persistent, so a missed weekday slot can be caught up after login.
The program itself rejects weekend collection, including timer catch-up. Reports
use London time with daylight-saving changes, irrespective of the host timezone.

## Manual checks

The optional fixture clock is compiled only with `testMode = true`; the installed
binary never honours it. From the dotfiles checkout:

```sh
test_package=$(direnv exec . nix build --impure --no-link --print-out-paths --expr '
  let f = builtins.getFlake "git+file:///home/william/.dotfiles";
      pkgs = f.inputs.nixpkgs.legacyPackages.x86_64-linux;
  in import ./programs/newrelic-brief { inherit pkgs; testMode = true; }')
direnv exec . python3 programs/newrelic-brief/tests/check.py "$test_package/bin/newrelic-brief"
```

Fixtures cover novelty transitions, private storage, Pi isolation flags, a real
PTY pager journey, unavailable/partial checks, terminal-control rejection,
weekends and five-day expiry. They make no network or model calls.
