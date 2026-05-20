---
name: local-dev-environment
description: Local machine and dotfiles context. Use for Will's dotfiles, Nix/Home Manager, direnv, Brave debugging, or machine-specific assumptions.
---

# Local Dev Environment

Use this for machine-specific workflow details.

When changing harness instructions, tools, extensions, skill discovery or payload budgets, read `~/.agents/guides/harness-cost.md`. Ordinary project tasks do not require it.

Key facts:
- Dotfiles repo: `~/.dotfiles`.
- Flake: `~/.dotfiles/flake.nix`.
- Home Manager package list: `~/.dotfiles/home/user/packages.nix`.
- Program config: `~/.dotfiles/home/user/programs.nix`.
- Projects generally use Nix plus direnv for dependencies and services.
- For project commands, prefer `direnv exec <project-path> <command>` or run from the project after direnv activation.
- Do not assume services are stopped just because a connection fails; the project environment may not be activated.
- Brave remote debugging is expected on `http://127.0.0.1:9222` when Brave is running.

**Home attribute (always explicit on every host):** `homeConfigurations` exposes platform-level attributes (`william-darwin`, `william-linux`) and per-host attributes (`william@andromeda`, `william@foundation`, `william@starfish`, `william@terminus`). The bare `william` attribute defaults to the Linux config and will produce confusing x86_64-linux build failures (adw-gtk3 fish-completions, dconf-keys) on a Mac. Prefer the `hmswitch` wrapper for activation; it chooses the right `nh home switch . --configuration ...` value for the current machine. Pick the right one for manual builds — don't guess; verify with `nix eval '.#homeConfigurations' --apply 'builtins.attrNames'` if unsure:

```sh
# macOS host:
nix build .#homeConfigurations.william-darwin.activationPackage
nh home switch . --configuration william-darwin

# Linux host:
nix build .#homeConfigurations.william-linux.activationPackage
nh home switch . --configuration william-linux

# Per-host (if a `william@<hostname>` attribute is defined for this machine):
nix build .#homeConfigurations."william@$(hostname)".activationPackage
nh home switch . --configuration "william@$(hostname)"
```

`nh home switch` takes the flake path as the installable and the Home Manager attr through `--configuration`. **Never** use `nh home switch '.#william-darwin'`: `nh` treats that as a package-style attr lookup instead of a `homeConfigurations` selection.

Read each project's environment and runbook for its local service ports.
