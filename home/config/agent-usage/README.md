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
`auth.json` atomically at mode 0600. OpenCode Go uses the environment key first,
then Pi's models configuration, then its auth registry. A configured `!command`
is trusted shell configuration, executed with a 20-second timeout. Keys and
provider diagnostics never appear in display records or logs.

Provider URLs are fixed, authenticated requests never follow redirects, TLS checks
both certificate trust and hostname, and probes retain a 15-second connection/idle
timeout without imposing a whole-response deadline. `SSL_CERT_FILE` selects the CA bundle; Nix supplies its
public certificate store by default.

`checks.<system>.agent-usage` compiles all four commands and runs the TypeScript
fixtures against their C implementation, including actual local HTTP/TLS servers.
The fixture executable is test-only and is not installed. Tests never use real
credentials or live provider APIs.
