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

Build with `nix build .#github-watch`. Manual migration fixtures live in `tests/`.
Configure Meson with `-Dfixtures=true`, then run `tests/watch.test.ts` with
`GITHUB_WATCH_BIN` and `GITHUB_WATCH_FIXTURE` pointing at that build. Fixtures use
fake CLI programs and temporary homes, not real credentials or notifications.
Nothing registers them as flake, package-build or commit-hook checks.
