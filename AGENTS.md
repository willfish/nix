# AGENTS.md — .dotfiles

This is William's personal NixOS + Home Manager dotfiles repository.

## Overview

- Multi-host NixOS configuration managed with flakes
- Heavy use of Home Manager for user environment
- Mix of upstream packages, custom overlays, and personally maintained tools
- Strong focus on developer ergonomics and AI agent tooling

## Hosts

- **Andromeda** — Thelio Major Threadripper desktop (main workstation)
- **Starfish** — Dell Precision 5750 laptop
- **Foundation** — Framework 13 AMD AI-300 Series laptop

## Key Directories

- `flake.nix` — Main flake with inputs, outputs, and host configurations
- `home/user/` — Home Manager configuration (packages.nix, programs.nix, shells, etc.)
- `home/config/` — Static files symlinked into `~` and `~/.config` (nvim, fish, cosmic, etc.)
- `home/user/modules/` (if present) — Reusable Home Manager modules

## Development Workflow

- Prefer small, focused changes
- Test with `nix build .#homeConfigurations.<host>.activationPackage` before switching
- Use `nh home switch` or `home-manager switch --flake .#william` for user changes
- Full system rebuilds via `nh os switch` or `sudo nixos-rebuild switch --flake .`
- Run `nix flake check` when touching flake.nix or major module changes

## AI / LLM Agent Setup

This repository has a well-developed Grok CLI harness:

- Native skills: `superpowers`, `systematic-debugging`, `verification-before-completion`, `writing-plans`
- High-quality reference library in `~/.grok/skills/references/` (Superpowers, plugin-eval, React/Supabase best practices)
- Global rules deployed to `~/.grok/AGENTS.md` and `~/.claude/CLAUDE.md`
- The root `AGENTS.md` contains repo-specific guidance

When working in this directory, always follow the rules in the global `~/.grok/AGENTS.md` + this file.

## Other Notes

- Fish is the primary shell
- Cosmic DE configuration is managed declaratively
- Several personal tools (`sniffy`, `smailer`, `mux`, `forte`, etc.) are developed alongside this repo
- Heavy use of `nh` for nicer nixos-rebuild / home-manager UX

---

**Global agent rules** (harness discipline, debugging process, verification, planning, etc.) live in the managed file at `home/config/grok/AGENTS.md` and are deployed to `~/.grok/AGENTS.md`.
