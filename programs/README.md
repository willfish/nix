# Owned programs

Repository-owned C and Rust programs live here, regardless of which host or
Home Manager module installs them. Each program keeps its source, `default.nix`,
build definition and README together. Checks stay program-local; the existing
multi-command checks for the two compatibility collections live with those
collections. Small programs may use a flat source layout; larger ones use `src/`
and `include/`. Do not add directories
or change build systems merely to make every tree look identical.

`home/config/` holds static configuration and harness assets. `home/user/` and
`system/` own deployment, service units, credentials and integration wrappers.
Upstream package definitions remain in their integration layer; this directory
is not a vendor tree.

## Voice client and service

- [pi-voice-client](pi-voice-client) owns the desktop UI, recording/playback,
  controls and Pi/Herdr session integration. Public commands remain `pi-voice`,
  `pi-voice-osd` and the existing launchers. The `*-c` executables are private
  implementation entry points behind those wrappers.
- [voice-api](voice-api) exposes a local Deepgram-compatible REST API and manages
  the underlying speech engines. The client can also use configured remote
  providers, and the API can serve other compatible clients.

## Find a program

Platforms describe intended runtime scope, not a claim of native-host testing.
Read each program's README for its authorization boundary and manual checks.

| Source/package | Purpose | Existing commands or entry point | Scope |
| --- | --- | --- | --- |
| [agent-usage](agent-usage) | Provider allowances and local LLM usage | `omarchy-agent-usage-*`, `hypr-agent-status` | Unix |
| [arxiv-status](arxiv-status) | Cached arXiv notification status | `hypr-arxiv-status` | Desktop |
| [audiobook-library](audiobook-library) | Inspect source inventories and destination duplicates | `qbittorrent-inventory`, `source-target-duplicate-check`, `libation-inventory` | Unix |
| [audit-skills](audit-skills) | Audit skill metadata and deployment references | `audit-skills` | Unix |
| [assistant-tools](assistant-tools) | Scoped filesystem and web MCP service | `local-assistant-tools` | Unix |
| [aws-access-portal](aws-access-portal) | Authorized AWS portal login and credential export | MCP wrapper | Unix |
| [check-flake-lock-update](check-flake-lock-update) | Validate lock updates against the owner policy | `check-flake-lock-update` | Unix |
| [color-contrast](color-contrast) | Calculate color contrast ratios | `contrast` | Unix |
| [daily-agenda](daily-agenda) | Calendar fetching, recurrence and reminders | `daily-agenda` | Unix |
| [daily-launcher](daily-launcher) | Launch workspace, notes, agenda and confirmed cleanup | `daily-workflow` | Linux desktop |
| [darwin-deployment](darwin-deployment) | macOS deployment, preflight and health | `darwin-deploy`, `darwin-preflight`, `darwin-health` | macOS |
| [github-notification-watch](github-notification-watch) | GitHub notification alerts | `github-notification-watch` | Desktop |
| [herdr-notification-focus](herdr-notification-focus) | Focus the session associated with a notification | `herdr-notification-focus` | Linux desktop |
| [herdr-sleep-inhibit](herdr-sleep-inhibit) | Prevent sleep while Herdr agents are working | `herdr-agent-awake` | Linux |
| [hermes-config](hermes-config) | Export and apply Hermes configuration and routing | `hermes-export`, `hermes-declaration`, `hermes-profile`, `hermes-telegram-route` | Relay deployment |
| [import-omarchy-community](import-omarchy-community) | Import reviewed community-theme catalogue data | `import-omarchy-community` | Unix |
| [launcher-projects](launcher-projects) | Discover and open project workspaces | `launcher-projects` | Linux desktop |
| [login-theme-apply](login-theme-apply) | Publish the selected login-screen theme | `greeter-select` | NixOS system service |
| [memscope](memscope) | Report RAM use and proportional mapped-file memory | `memscope` | Linux |
| [nix-storage-report](nix-storage-report) | Account for direct, shared and unique store space | `nix-storage-report` | Unix |
| [nm-auto-secret-agent](nm-auto-secret-agent) | Supply configured NetworkManager secrets | User secret-agent service | Linux |
| [omapager-tools](omapager-tools) | Prepare and integrate the upstream desktop panel | Build helpers and native icon module | Linux desktop |
| [opencode-adapt](opencode-adapt) | Adapt shared Markdown for OpenCode | `opencode-adapt-markdown` | Build helper |
| [panel-settings](panel-settings) | Publish calendar and weather settings | `hypr-calendar-settings`, `hypr-weather-settings` | Desktop |
| [personaplex-tools](personaplex-tools) | Prepare PersonaPlex models and upstream patches | `personaplex-models`, `personaplex-patch` | Voice setup |
| [pi-config](pi-config) | Merge Pi configuration and validate capture bus hosts | `pi-merge-auth`, `pi-merge-settings`, `pi-capture-bus-host` | Unix |
| [pi-token-report](pi-token-report) | Produce private token-usage reports from authorized captures | `pi-token-report` | Unix |
| [pi-voice-client](pi-voice-client) | Voice UI and session integration | `pi-voice`, `pi-voice-osd` wrappers | Linux desktop |
| [prompt-capture](prompt-capture) | Record opt-in LLM traffic through mitmproxy | `prompt-capture` wrapper | Unix |
| [slack-session](slack-session) | Refresh an authorized Slack browser session | `slack-refresh-session` | Unix |
| [tailscale-proxy](tailscale-proxy) | Tailnet-aware HTTP and WebSocket proxy | `tailscale-open-proxy` | Unix |
| [telegram-login](telegram-login) | Interactive Telegram session setup | `telegram-mcp-login` | Unix |
| [theme-menu](theme-menu) | Select and publish desktop themes | `theme-menu` | Linux desktop |
| [voice-api](voice-api) | Local REST adapter for speech engines | `pi-voice-api` | Local speech host |
| [voice-models](voice-models) | Install verified speech model assets | `voice-model-setup` | Voice setup |
| [wallpaper-cycle](wallpaper-cycle) | Select and prepare the next wallpaper | `wallpaper-cycle` | Desktop |
| [youtube-extract](youtube-extract) | Extract metadata and subtitle text without downloading media | `youtube-extract` | Unix |

## Names and compatibility

Name programs for their job, not their language or the caller that happens to
use them. Keep cohesive product families together. A source/package rename does
not require changing a public command or service ID.

The old flake attributes `github-watch`, `daily-workflow`, `greeter-select`,
`hermes-tools` and `darwin-tools` remain aliases of the clearer package names.
For example, both `.#daily-launcher` and `.#daily-workflow` provide the existing
`daily-workflow` command. [repo-tools](collections/repo-tools) and
[skill-tools](collections/skill-tools) remain compatibility collections of the
individually named packages. No root service rollout is implied by a source move.

C programs follow [the C house standard](../docs/c-programs.md). Fixtures stay
program-local, manual, noninstalled and unregistered with package builds, flake
checks and hooks. Rust services retain locked Cargo dependencies.
