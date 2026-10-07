# GitHub desktop notifications

`github-notification-watch` makes one read-only `gh api --paginate notifications`
invocation and exits after its notification actions finish. GitHub CLI owns
credentials. The first successful poll summarizes the unread backlog; later
polls announce at most eight fresh threads plus an overflow count.

State stays at `$HOME/.local/state/github-notifications/seen.json`, independent
of XDG state overrides. Seen IDs accumulate and are sorted on atomic replacement.
Failed or malformed fetches leave state unchanged. No thread is marked read on
GitHub. Open actions accept only converted HTTPS pages on `github.com`; commands
use argv, never a shell. Polling, notification waits and browser launches retain
their 30-, 60- and 10-second timeouts.

Build with `nix build .#github-watch`. Manual fixtures are default-off, not
installed, and not registered as Meson, flake, package-build or hook checks.
From the repository root:

```sh
nix develop .#github-watch --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/github-watch-checks home/config/github-watch \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#github-watch --command meson compile -C /tmp/github-watch-checks
/tmp/github-watch-checks/watch-checks
```

`GITHUB_WATCH_BIN` and `GITHUB_WATCH_FIXTURE` override the sibling binaries.
The check uses a C stand-in for `gh`, `notify-send` and `xdg-open`, plus temporary
homes, not real credentials or notifications.
