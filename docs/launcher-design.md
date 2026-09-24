# Intelligent launcher

Walker is the search UI; Elephant supplies apps, live windows, projects and desktop
actions. `hypr-launcher` remains the entry point, with Fuzzel as the recovery path.
The existing theme switcher, voice picker, Waybar and direct app bindings remain.

Here, intelligent means finding useful results with little typing, local usage
ranking and explicit per-result actions. An LLM is not on the app-launch path.

## Use

Open with the existing Super or Super+X binding. Type normally to search across
apps, windows, projects and actions, or narrow the source:

| Prefix | Source |
| --- | --- |
| `=` | Calculator |
| `$` | Running windows |
| `/` | Projects |
| `:` | Desktop actions |
| `;` | Provider list |

Project results open a terminal with Enter, Neovim with Ctrl+Enter, or the file
manager with Alt+Enter. App results expose their available actions in the footer.
Search `theme` or `voice` to open the existing pickers. Session actions open the
existing session menu rather than immediately rebooting or logging out.

Qwen chat actions open the configured browser endpoints on Andromeda or Relay.
They do not send the launcher query to a model or ensure the model server is up.
Semantic knowledge-base search, web search and model-generated actions are not
implemented.

## Why this stack

The pinned [Omarchy revision, 28ceaae](https://github.com/basecamp/omarchy/tree/28ceaae70ebac3a0edcc21f2faa77a90dc6d404c),
uses a Quickshell menu, not Walker. Its
[Menu.qml](https://github.com/basecamp/omarchy/blob/28ceaae70ebac3a0edcc21f2faa77a90dc6d404c/shell/plugins/menu/Menu.qml)
combines actions with an Apps provider. The app list starts alphabetically;
menu matching is not usage-ranked cross-provider search. Its restrained card,
readable type, icons and accent selection are useful design references without
porting its Arch installers or entire shell.

Older Omarchy used Walker and Elephant. We use the locked Walker 2.16.2 and
Elephant 2.21.0 with a richer provider configuration, not a verbatim historical
Omarchy configuration. No flake-input upgrade is required.

- **Walker + Elephant:** unified local search and an independently themed UI;
  costs two resident services and provider integration.
- **Vicinae:** Raycast-like extensions and richer actions; different visuals,
  with extension compatibility and trust requiring individual review.
- **Omarchy Quickshell menu:** exact current Omarchy visuals; ranking and
  additional sources become custom code.
- **Fuzzel:** minimal focused pickers; unified search and per-result actions
  require custom orchestration.
- **Anyrun / Rofi:** plugin search or scripted modes; less direct fit for this
  provider-based design.

## Ownership and safety

- `home/user/launcher.nix` owns packages, providers and services on graphical
  Linux only. Both services belong to `hyprland-session.target`. Walker's service
  PATH includes Elephant because the frontend invokes its CLI as well as using
  its socket.
- Elephant 2.21.0 reads `elephant.toml`; this Home Manager revision would otherwise
  generate `config.toml`. Generate the correct filename explicitly.
- `home/user/themes/hyprland.nix` renders complete Walker CSS from the existing
  Base16 palette and fonts. Walker imports the active theme-state asset. Theme
  publication restarts an already-running Walker so it reloads CSS; an open
  launcher may close. Fuzzel assets remain for the other pickers and recovery.
- Project discovery reads directory metadata only, under `~/Repositories` and
  `~/.dotfiles`. It recognises `.git` directories and worktree marker files,
  skips symlinks and dependency/hidden directories, and stops at depth 3,
  200 projects or 4000 directory entries. The bounded scan runs on each query.
  Discovery never executes Git, direnv or repository code. Opening a terminal
  still runs the user's normal shell, including its existing direnv policy.
- Project values are opaque SHA-256 IDs, resolved again before execution. Paths
  stay argv values, never shell fragments. Application and project launches use
  separate user scopes so Elephant restarts do not kill their processes.
- Elephant's desktop-entry execution remains upstream behavior, but a local
  patch rejects query-supplied app arguments: the upstream `#` suffix must not
  become shell code. Preserve this guard when upgrading Elephant.
- Clipboard capture, shell runner, browser history, network search and broad
  file indexing are not loaded. Usage history stays local. Do not enable private
  sources without explicit roots, exclusions and retention decisions.

## Recovery and checks

`hypr-launcher --fallback` opens Fuzzel directly. Startup, backend probing and
frontend invocation have bounded waits and fall back on failure.

```sh
systemctl --user status walker elephant
journalctl --user -u walker -u elephant
systemctl --user restart elephant walker
```

Run launcher tests with `direnv exec . python3 -m unittest discover -s tests -p
 'test_launcher_*.py' -v`, and evaluate host boundaries with `direnv exec . nix eval
 --impure --json --file tests/launcher-config.nix`. Build and activate the selected
host with `hmswitch`, then verify real provider queries and UI behavior.

Before expanding deployment, check window focus and new-instance actions,
light/dark reloads, multiple monitors, login/logout, and cold/warm latency on the
actual host. Fuzzy ranking is native provider behavior, not a custom global
relevance model. Add scoped knowledge-base retrieval only if it improves daily
use without putting model latency or generated shell commands on the launch path.
