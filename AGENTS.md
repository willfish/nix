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
- Branch names should use the relevant ticket or `BAU` prefix, never an agent/tool prefix like `codex/`
- Test with `nix build .#homeConfigurations.<host>.activationPackage` before switching
- Bare `homeConfigurations.william` defaults to the Linux Home Manager config; use `william-darwin` explicitly on macOS.
- Use `nh home switch` or `home-manager switch --flake .#william` for user changes
- Full system rebuilds via `nh os switch` or `sudo nixos-rebuild switch --flake .`
- Run `nix flake check` when touching flake.nix or major module changes

## AI / LLM Agent Setup

This repository has a well-developed multi-TUI LLM harness:

- Universal rules from `home/config/llm/AGENTS.md` deployed to Grok, Claude Code, Codex, Gemini, `.agents`, and Antigravity fallback paths
- Shared job-specific skills from `home/config/llm/skills/`
- Shared guides from `home/config/llm/guides/`
- Process skills and the broader reference library from `home/config/llm/process-skills/` and `home/config/llm/references/`
- The root `AGENTS.md` contains repo-specific guidance

When working in this directory, always follow the universal harness rules plus this repo-local file.

## Other Notes

- Fish is the primary shell
- Cosmic DE configuration is managed declaratively
- Several personal tools (`sniffy`, `smailer`, `mux`, `forte`, etc.) are developed alongside this repo
- Heavy use of `nh` for nicer nixos-rebuild / home-manager UX

---

**Global agent rules** (harness discipline, debugging process, verification, planning, etc.) live in the managed file at `home/config/llm/AGENTS.md` and are deployed to the supported TUI config directories via Home Manager.

---

## Dotfiles-Specific Guidance (NixOS / Home Manager)

When working inside this repository, the following conventions apply in addition to the universal harness rules.

### Hosts

- **Andromeda** — Thelio Major Threadripper desktop (main workstation)
- **Starfish** — Dell Precision 5750 laptop
- **Foundation** — Framework 13 AMD AI-300 Series laptop

### Key Directories & Files

- `flake.nix` — Main flake with inputs, outputs, and host configurations
- `home/user/` — Home Manager configuration (packages.nix, programs.nix, shells.nix, config.nix, etc.)
- `home/config/` — Static files symlinked into `~` and `~/.config` (nvim, fish, cosmic, llm harness, etc.)
- `system/` — Per-host NixOS configuration (andromeda/, starfish/, foundation/)

### Development Workflow (this repo)

- Prefer small, focused changes.
- Test with `nix build .#homeConfigurations.<host>.activationPackage` (or the appropriate attribute) before switching.
- Use `nh home switch .` on Linux; `nh` should auto-detect `william@<hostname>` and fall back to the Linux `william` config. Use `william-darwin` explicitly on macOS.
- Full system rebuilds via `nh os switch` or `sudo nixos-rebuild switch --flake .`.
- Run `nix flake check` when touching `flake.nix` or major module changes.
- After changes to Home Manager modules, run the activation and verify before committing.

### Editing Conventions (Nix / Home Manager)

- Prefer small, focused changes. Run `git diff` (mentally or actually) before committing.
- After editing `.nix` files under `home/user/`, test with a build:
  `nix build .#homeConfigurations.william.activationPackage` (Linux default) or a host-specific attribute such as `william@andromeda`.
- For additions to `programs.nix` or `packages.nix`, ensure the package exists in the flake inputs or nixpkgs.
- Shell configuration lives in `shells.nix` + `programs.nix`. Fish is the primary shell.
- Secrets / private data: never commit. Use `agenix` (or equivalent) if present; otherwise keep out of git.

### Browser Integration (for agents with browser tools)

Brave is configured with remote debugging (`--remote-debugging-port=9222`) in `home/user/programs.nix`.

- Verify: `curl -s http://127.0.0.1:9222/json/version`
- Use `brave-devtools` MCP tools (or equivalent) for tab interaction, navigation, screenshots, etc.
- Prefer `gh` CLI for GitHub data over scraping to save tokens.
- Use `evaluate_script` for extracting page content instead of full snapshots when possible.

### Local Package Overlays

- `~/Repositories/nixpkgs` is used **only** for overlaid packages (e.g. `variety`).
- Custom LLM agent packages (`grok-cli`, `codex`, `claude-code`) are managed via the `llm-agents.nix` flake input.

### AI / LLM Agent Setup in This Repo

This repository maintains a well-developed, multi-TUI harness:

- **Universal harness**: `home/config/llm/AGENTS.md` (deployed everywhere)
- **Job-specific guides**: `home/config/llm/guides/` (Jira, PRs, voice, epics/stories, RSpec, trade-tariff frontend, etc.)
- **Shared job-specific skills**: `home/config/llm/skills/` (hmrc-trade-tariff-workflow, jira-workflow, will-voice, etc.)
- **Process skills and reference library**: `home/config/llm/process-skills/` + `home/config/llm/references/` (superpowers, systematic-debugging, verification-before-completion, writing-plans, create-skill, references)
- **Repo-local guidance**: This file (the one you are reading)

When working in this directory, always follow the rules in the universal `~/.grok/AGENTS.md` (or `~/.claude/CLAUDE.md`) **plus** this file.

### Other Notes

- Fish is the primary shell.
- Cosmic DE configuration is managed declaratively (shortcuts, panel, dock, themes, autotile, etc.).
- Several personal tools (`sniffy`, `smailer`, `mux`, `forte`, etc.) are developed alongside this repo.
- Heavy use of `nh` for nicer `nixos-rebuild` / `home-manager` UX.

---

*Single-source LLM harness content (universal rules, shared guides, and shared job skills) lives under `home/config/llm/`. The old tool-specific global AGENTS files have been retired in favour of the clean universal version + this repo-local document.*
