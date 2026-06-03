---
name: local-dev-environment
description: Use when working with Will's dotfiles, Nix/Home Manager setup, direnv project environments, Brave remote debugging, local trade-tariff ports, or local service assumptions on this machine.
---

# Local Dev Environment

Use this for machine-specific workflow details.

Key facts:
- Dotfiles repo: `~/.dotfiles`.
- Flake: `~/.dotfiles/flake.nix`.
- Home Manager package list: `~/.dotfiles/home/user/packages.nix`.
- Program config: `~/.dotfiles/home/user/programs.nix`.
- Projects generally use Nix plus direnv for dependencies and services.
- For project commands, prefer `direnv exec <project-path> <command>` or run from the project after direnv activation.
- Do not assume services are stopped just because a connection fails; the project environment may not be activated.
- Brave remote debugging is expected on `http://127.0.0.1:9222` when Brave is running.

**Home attribute (always explicit on every host):** `homeConfigurations` exposes platform-level attributes (`william-darwin`, `william-linux`) and per-host attributes (`william@andromeda`, `william@foundation`, `william@starfish`). The bare `william` attribute defaults to the Linux config and will produce confusing x86_64-linux build failures (adw-gtk3 fish-completions, dconf-keys) on a Mac. Pick the right one for the current host — don't guess; verify with `nix eval '.#homeConfigurations' --apply 'builtins.attrNames'` if unsure:

```sh
# macOS host:
nix build .#homeConfigurations.william-darwin.activationPackage
nh home switch '.#william-darwin'

# Linux host:
nix build .#homeConfigurations.william-linux.activationPackage
nh home switch '.#william-linux'

# Per-host (if a `william@<hostname>` attribute is defined for this machine):
nix build .#homeConfigurations."william@$(hostname)".activationPackage
nh home switch ".#william@$(hostname)"
```

`nh home switch .` (no attribute) also works because `nh` auto-detects from the current host, but the explicit form is the safer reflex when working on a new box. **Never** `nh home switch #william-darwin` (or any other `#attr` form) without a leading `.` — the shell treats `#` as a comment character and the suffix gets dropped, breaking the command silently.

Trade Tariff local ports:
- backend: 3000
- frontend: 3001
- duty-calculator: 3002
- admin: 3003
- dev-hub: 3004
- identity: 3005
