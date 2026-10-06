# Handwritten Python migration

Replace repository-owned Python utilities with compiled tools while preserving
command names, configuration, output formats and host boundaries. Use Rust for
HTTP, WebSocket and MCP services; use C for small command-line and desktop
helpers. Third-party Python runtimes, including moshi/personaplex, DDGS and
telegram-mcp, are not ports in this plan.

## Execution

Work through the queue in order, one bounded utility or tightly coupled family
at a time. Implement first, then verify the candidate executable. Optional
migration fixtures belong inside each program's root and run manually, using
Cargo tests or compiled/simple drivers as appropriate. Do not add repository-wide
tests, flake checks, package test phases or commit-hook test gates. Live model
downloads, inference and remote activation are not prerequisites for verifying
an adapter.

For every completed port:

1. Replace its implementation and wire the compiled package into Nix.
2. Preserve its public contract and remove the retired handwritten Python.
3. Build the package, run relevant manual behavior/wiring checks and review the diff.
4. Commit only the port's files on master, run `hmswitch`, verify activation and
   push before starting the next port.

Build the selected package, not unrelated full host configurations. Evaluate
other hosts' wiring where necessary; record native-host coverage gaps rather than
claiming a local activation exercises a service disabled on that host. Use locked
Rust dependencies and existing C libraries where practical.

## Queue

### Services

- [x] Local voice REST API: Rust `home/config/voice-api`.
- [x] Tailscale authentication proxy: Rust `home/config/tailscale-proxy`.
  Preserve peer-address-only trust, caller credentials, streaming and WebSocket
  upgrades. Never put the relay key in the Nix store or logs.
- [x] Local assistant MCP service: Rust `home/config/assistant-tools`.
  Preserve read/write roots, credential exclusions and web-fetch restrictions.

### Desktop helpers

- [x] Agent usage/status family: C `home/config/agent-usage`, including
  the three collectors, status command and shared authentication/usage code.
- [x] arXiv status: C `home/config/arxiv-status`.
- [x] GitHub notifications: C `home/config/github-watch`.
- [x] Wallpaper cycling: C `home/config/wallpaper-cycle`.
- [x] Voice model setup: C `home/config/voice-models`.
- [x] PersonaPlex model/patch helpers: C `home/config/personaplex-tools`.
- [x] Theme menu: C `home/config/theme-menu`.
- [x] Launcher project discovery/opening: C `home/config/launcher-projects`.
- [x] Launcher daily workflow: C `home/config/daily-workflow`.
- [x] Launcher agenda: C `home/config/daily-agenda`, including private feed
  fetching, sandboxed recurrence expansion, reminders and both calendar caches.
- [x] Herdr notification focus: C `home/config/herdr-notification-focus`.
- [x] Omapager preparation/icon/patch helpers: C `home/config/omapager-tools`,
  including the compiled icon module for the unchanged upstream Python runtime.
- [x] Calendar/weather panel settings writers: C `home/config/panel-settings`.
- [x] Greeter selection: C `home/config/greeter-select` with unchanged root
  service confinement and catalogue-only asset selection.
- [x] NetworkManager secret agent: Rust `home/config/nm-auto-secret-agent` with a
  compiled libnm bridge. The existing Bash reconnect watcher is unchanged.

### Configuration, build and skill utilities

- [ ] Pi authentication/settings mergers and OpenCode Markdown adapter.
- [ ] Hermes declaration, export, profile and Telegram routing helpers.
- [ ] Repository lock-policy and community-import scripts; the test-only behavior gate is removed.
- [ ] Darwin deployment, preflight and health tools.
- [ ] AWS access-portal service and helpers. Preserve existing authorization gates.
- [ ] Skill audit, audiobook inventory/duplicate checks, contrast, token-report and
  YouTube extraction utilities.

### Final audit

- [ ] Remove obsolete repository-wide Python test drivers; retain only optional,
  program-local migration fixtures without automated wiring.
- [ ] Audit tracked `.py` files, extensionless commands, embedded Python in Nix and
  shell, launch references and development-only Python dependencies.
- [ ] Run the relevant complete check groups, reconcile documented exceptions,
  and finish the final commit, activation and push.

## Current coverage limits

The voice REST adapter, Tailscale proxy and assistant MCP service have
package/fixture coverage on Linux. Native Darwin execution, live search-provider
results and live GPU inference on Andromeda have not been verified. Foundation
activation does not run the local voice API or Relay-only services.
