# Herdr notification focus

One-shot C helper behind Omapager's Ghostty notification actions. `remember` saves
the pane, tab or workspace identified by a toast. `open` checks that saved target,
resolves stale targets again, focuses Herdr, then raises the corresponding window
through a fixed `hyprctl` argv. It does not run notification text as a command.

## Contract

- Match workspace labels before numbers, preserving ambiguous workspace/tab
  fallbacks and status/sequence-based pane selection. Cached panes remain usable
  after their agent status changes while the pane still exists.
- Discover the nonempty `HERDR_SOCKET_PATH` override, the default
  `$XDG_CONFIG_HOME/herdr/herdr.sock`, then sorted session sockets. Unset/empty
  XDG configuration falls back to `$HOME/.config`. Only existing paths are used.
- Use newline-delimited Herdr requests with the existing request ID. Connect and
  send operations have two-second deadlines; each receive operation has its own
  two-second idle deadline. A progressing response may take longer overall.
- Retain `$XDG_STATE_HOME/herdr-notification-focus/targets.json`, falling back to
  `$HOME/.local/state`, with exact notification keys, newest-first lookup and the
  latest 100 entries. Preserve unrelated cache metadata. Publish through the
  existing `.tmp` replacement with mode 0600 and new directories mode 0700.
- Prefer the explicit sender PID, attached Herdr process ancestry, a unique
  matching Ghostty title, then the first minimum focus-history value. Exclude
  Cliamp from Ghostty fallback, not from explicit sender/ancestor selection.
- Keep two-second compositor subprocess deadlines and reap timed-out children.
  Malformed selected records and transport failures fail the command; unrelated
  toasts and unavailable client probes retain their existing fallback behavior.

The graphical Home Manager boundary and Omapager QML hooks remain unchanged.
Upstream Omapager's Python runtime and the separate owned build/icon adapters are
not part of this command.

## Manual fixtures

Build with GLib/GIO, yyjson, Meson, Ninja and pkg-config in an ephemeral Nix shell
from the existing direnv checkout:

```sh
nix develop .#herdr-notification-focus --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/herdr-focus-checks programs/herdr-notification-focus \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#herdr-notification-focus --command meson compile -C /tmp/herdr-focus-checks
/tmp/herdr-focus-checks/focus-checks
```

`HERDR_FOCUS_BIN` and `HERDR_FOCUS_FIXTURE` still select those executables;
otherwise the checks use siblings from the same build. Configure once without
`-Dfixtures=true` to confirm the package build does not create the check
executables.

Fixtures are default-off, noninstalled and never registered as automatic tests.
They use disposable Unix peers, process metadata and a stub `hyprctl`; no test
focuses live windows. Set `HERDR_FOCUS_LEGACY` to an executable retained legacy
command for optional CLI parity. Repeat with a separate ASan/UBSan build for
memory checks. These fixture controls are not production command options.
