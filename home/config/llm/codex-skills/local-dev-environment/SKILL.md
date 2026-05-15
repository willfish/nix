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

Trade Tariff local ports:
- backend: 3000
- frontend: 3001
- duty-calculator: 3002
- admin: 3003
- dev-hub: 3004
- identity: 3005
