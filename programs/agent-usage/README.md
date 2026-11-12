# Agent usage helpers

C replacements for `hypr-agent-status` and
`omarchy-agent-usage-{claude,codex,grok,opencode,copilot,antigravity}`, built with Meson, yyjson, libcurl
and GLib. The updater's `--force` and `--limits-only` flags remain accepted.

Collectors read Pi assistant usage under `${PI_AGENT_DIR:-$HOME/.pi/agent}/sessions`
and emit the pinned Omarchy panel's schema-version-1 records. Message content is
not included. Dates follow the local timezone; allowance reset timestamps use UTC.
The status command reads sorted JSON records from
`${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/agents/usage`.

Claude reads `${CLAUDE_CONFIG_DIR:-$HOME/.claude}/.credentials.json` without
copying, refreshing or rewriting credentials. It shows account-wide five-hour,
weekly and available model-specific subscription limits with reset times, not
local token or session totals. Missing limits remain unavailable, not zero.
Claude Code owns token refresh; start it when the widget reports paused limits.

Pi owns the other providers' credentials. OAuth refresh preserves profile fields and replaces
`auth.json` atomically at mode 0600. A failed save produces an explicit status and
stops the usage probe rather than using credentials that were not persisted.
These short-lived commands terminate with a value-free diagnostic on allocation
failure or allocation-size overflow, including JSON construction.
OpenCode Go uses the environment key first,
then Pi's models configuration, then its auth registry. A configured `!command`
is trusted shell configuration, executed with a 20-second timeout. Keys and
provider diagnostics never appear in display records or logs.

Copilot reads the GitHub OAuth token Pi stores for `github-copilot` and asks
GitHub's `copilot_internal/user` endpoint for monthly quotas. Limited quotas
become meters. GitHub reports metered seats as unlimited with no entitlement, so
that case becomes a `usageNoteText` line with the credits used, not a meter.
Unlimited means no seat quota, not free usage; billing budgets are not visible.

Antigravity reads model quotas and reset times from the signed-in IDE's local
`GetUserStatus` service. It discovers only the current user's Antigravity language
server and its listening sockets through `/proc`. Requests stay on loopback,
bypass proxies, and never copy, refresh or rewrite Google credentials. The app
must be running with a project open. Closed, signed-out or incompatible app
versions show unavailable limits, not zero. Only measured model allowances are
shown; weekly caps, token totals and AI credit spend are not inferred. Google AI
Pro eligibility does not establish the signed-in account's actual plan; the plan
label comes from the app.

Provider URLs are fixed, authenticated requests never follow redirects, TLS checks
both certificate trust and hostname, and probes retain a 15-second connection/idle
timeout without imposing a whole-response deadline. `SSL_CERT_FILE` selects the CA bundle; Nix supplies its
public certificate store by default.
The Antigravity loopback service alone permits its self-signed certificate and
uses a two-second request deadline; remote provider TLS verification is unchanged.

`nix build .#agent-usage` compiles all seven commands without running fixtures.
For manual migration verification, configure Meson with
`-Dfixtures=true --buildtype=debugoptimized`, then
run that build's `usage-checks` executable. The fixture executable is not installed. Fixtures never
use real credentials or live provider APIs; they are not flake or hook checks.
