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

- [x] Pi authentication/settings mergers and capture bus-host parser:
  C `home/config/pi-config`.
- [x] OpenCode Markdown adapter: C `home/config/opencode-adapt`.
- [x] Hermes exporter, declaration installer, profile and Telegram routing:
  C `home/config/hermes-tools`.
- [x] Repository lock-policy, community importer and storage-cost accounting:
  C `home/config/repo-tools`.
  The test-only behavior gate remains removed.
- [x] Darwin deployment, preflight and health tools: C `home/config/darwin-tools`.
  Native macOS deployment and recovery remain separately authorized operations.
- [x] Slack session refresh: Rust `home/config/slack-session`, retaining private
  publication, workspace/tab ownership and optional SOPS updates.
- [x] AWS access-portal service and helpers: Rust `home/config/aws-access-portal`,
  retaining explicit login scope and local production-administrator approval.
- [x] Contrast, YouTube extraction, audiobook inventory/duplicate checks and
  Libation index helper: C `home/config/skill-tools`.
- [x] Skill audit: C `home/config/repo-tools/audit.c`.
- [x] Token-report utility: C `home/config/skill-tools`. Its fixed, model-generated
  Python benchmark workload is retained for comparable captures.
- [x] Prompt-capture MITM add-on: C `home/config/prompt-capture`, retaining the
  upstream mitmproxy runtime, transparent streams and private JSONL records.
- [x] Telegram manual-login adapter: C `home/config/telegram-login`, using the
  unchanged upstream Telethon runtime through the CPython API.

### Final audit

- [x] Remove obsolete repository-wide Python test drivers; retain only optional,
  program-local migration fixtures without automated wiring.
- [x] Audit tracked and ignored source files, extensionless commands, embedded
  Python in Nix and shell, launch references and development-only dependencies.
- [x] Verify each delivery's relevant complete check groups, reconcile retained
  dependencies, and release through commit, Home Manager activation and push.

The `memscope` CLI, terminal and live-memory checks are C and run only through
its explicit local `make check` target, not package builds.

## Retained dependencies and data

The maintained repository utilities no longer contain handwritten Python.
Remaining Python references serve these distinct purposes:

- Upstream runtimes: PersonaPlex/moshi, DDGS, Hermes, Strata, telegram-mcp,
  mitmproxy, Omapager and Omarchy helpers. Their packages and upstream patches
  remain dependencies, not owned replacement implementations.
- Native interoperability: the Telegram, Omapager and prompt-capture C adapters
  call upstream APIs through CPython; they do not generate Python glue.
- General tooling: the user's Python/uv installation, Neovim support and upstream
  build dependencies such as ttfx. The obsolete migration-era Python development
  package bundle is removed from the flake shell.
- Data and references: generic documentation examples, patch matching text,
  optional comparisons against externally supplied historical executables, and
  the token reporter's fixed model-generated benchmark. Private-repository
  tooling is outside this checkout's scope.

Ignored historical design experiments are preserved outside the checkout, with
an archive pointer under `plans/`. Orphaned generated bytecode is removed rather
than treated as maintained source.

## Coverage limits

Program-local checks use disposable data and local stub peers. Linux package
builds and Foundation Home Manager activation do not establish native Darwin or
Relay operation, live provider results, credential refresh/export, or GPU model
inference. Those remain separately authorized operations. Home Manager also does
not perform root NixOS activation; use the host-operation workflow for system
changes.
