# Agent usage helpers

C replacements for `hypr-agent-status` and
`omarchy-agent-usage-{codex,grok,opencode}`, built with Meson, yyjson, libcurl
and GLib. The updater's `--force` and `--limits-only` flags remain accepted.

Collectors read Pi assistant usage under `${PI_AGENT_DIR:-$HOME/.pi/agent}/sessions`
and emit the pinned Omarchy panel's schema-version-1 records. Message content is
not included. Dates follow the local timezone; allowance reset timestamps use UTC.
The status command reads sorted JSON records from
`${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/agents/usage`.

Pi owns provider credentials. OAuth refresh preserves profile fields and replaces
`auth.json` atomically at mode 0600. A failed save produces an explicit status and
stops the usage probe rather than using credentials that were not persisted.
These short-lived commands terminate with a value-free diagnostic on allocation
failure or allocation-size overflow, including JSON construction.
OpenCode Go uses the environment key first,
then Pi's models configuration, then its auth registry. A configured `!command`
is trusted shell configuration, executed with a 20-second timeout. Keys and
provider diagnostics never appear in display records or logs.

Provider URLs are fixed, authenticated requests never follow redirects, TLS checks
both certificate trust and hostname, and probes retain a 15-second connection/idle
timeout without imposing a whole-response deadline. `SSL_CERT_FILE` selects the CA bundle; Nix supplies its
public certificate store by default.

`nix build .#agent-usage` compiles all four commands without running fixtures.
For manual migration verification, configure Meson with `-Dfixtures=true`, then
run `tests/usage.test.ts` with `AGENT_USAGE_FIXTURE` and `AGENT_USAGE_BIN_DIR`
pointing at that build. The fixture executable is not installed. Fixtures never
use real credentials or live provider APIs; they are not flake or hook checks.
