# Handwritten Python migration

Replace repository-owned Python utilities with compiled tools while preserving
command names, configuration, output formats and host boundaries. Use Rust for
HTTP, WebSocket and MCP services; use C for small command-line and desktop
helpers. Third-party Python runtimes, including moshi/personaplex, DDGS and
telegram-mcp, are not ports in this plan.

## Execution

Work through the queue in order, one bounded utility or tightly coupled family
at a time. Implement first, then migrate its Python tests using the best fit:
Cargo unit/integration tests for Rust and compiled tests or simple drivers for C.
There is no required test language. Run the complete relevant checks against the
candidate executable. Keep fixtures local; live model downloads, inference and
remote activation are not prerequisites for testing an adapter.

For every completed port:

1. Replace its implementation and wire the compiled package into Nix.
2. Preserve its public contract and remove the retired handwritten Python.
3. Run package tests and affected integration/wiring tests, then review the diff.
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

- [ ] Agent usage/status family: `home/config/hyprland/agents`, including
  extensionless Python commands and their shared authentication/usage code.
- [ ] arXiv status: `home/config/hyprland/arxiv/status.py`.
- [ ] GitHub notifications: `home/config/hyprland/omapager/github_watch.py`.
- [ ] Wallpaper cycling: `home/config/hyprland/wallpaper_cycle.py`.
- [ ] Voice model setup and personaplex model/patch helpers in `home/config/voice`.
- [ ] Theme menu: `home/config/appearance/theme_menu.py`.
- [ ] Launcher projects, daily and agenda helpers in `home/config/launcher`.
- [ ] Herdr notification focus and omapager preparation/icon/patch helpers.
- [ ] Greeter selection and embedded NetworkManager/panel Python helpers.

### Configuration, build and skill utilities

- [ ] Pi authentication/settings mergers and OpenCode Markdown adapter.
- [ ] Hermes declaration, export, profile and Telegram routing helpers.
- [ ] Repository behaviour, lock-policy and community-import scripts.
- [ ] Darwin deployment, preflight and health tools.
- [ ] AWS access-portal service and helpers. Preserve existing authorization gates.
- [ ] Skill audit, audiobook inventory/duplicate checks, contrast, token-report and
  YouTube extraction utilities.

### Final audit

- [ ] Port remaining Python test drivers, including memscope and fresh-install
  harnesses, without replacing unrelated third-party runtimes.
- [ ] Audit tracked `.py` files, extensionless commands, embedded Python in Nix and
  shell, launch references and development-only Python dependencies.
- [ ] Run the relevant complete check groups, reconcile documented exceptions,
  and finish the final commit, activation and push.

## Current coverage limits

The voice REST adapter, Tailscale proxy and assistant MCP service have
package/fixture coverage on Linux. Native Darwin execution, live search-provider
results and live GPU inference on Andromeda have not been verified. Foundation
activation does not run the local voice API or Relay-only services.
