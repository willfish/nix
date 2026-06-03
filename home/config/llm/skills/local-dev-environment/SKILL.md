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

**macOS home attribute (always explicit):** on Apple Silicon (aarch64-darwin), use `homeConfigurations.william-darwin` — not `homeConfigurations.william`. The bare `william` attribute defaults to the Linux config and produces confusing x86_64-linux build failures (adw-gtk3 fish-completions, dconf-keys) on a Mac. The explicit forms are:

```sh
# Build:
nix build .#homeConfigurations.william-darwin.activationPackage

# Switch (the suffix is a flake attribute, NOT a nh flag — quote the whole arg):
nh home switch '.#william-darwin'
```

`nh home switch .` (no attribute) also works because `nh` auto-detects, but the explicit form is the safer reflex. **Never** `nh home switch #william-darwin` without a leading `.` — the shell treats `#` as a comment character and the suffix gets dropped, breaking the command silently.

Trade Tariff local ports:
- backend: 3000
- frontend: 3001
- duty-calculator: 3002
- admin: 3003
- dev-hub: 3004
- identity: 3005
