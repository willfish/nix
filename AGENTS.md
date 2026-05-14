# AGENTS.md - .dotfiles Repository Rules

This file provides **repository-specific** instructions for Grok CLI, Claude Code, Codex, and compatible agents when the working directory is inside `~/.dotfiles`.

**Global rules** (the full harness discipline, skill-authoring guidelines, NixOS notes, browser integration, etc.) live in the managed file at:

- `~/.grok/AGENTS.md` (deployed from `home/config/grok/AGENTS.md`)
- `~/.claude/CLAUDE.md` (same content, for Claude compatibility)

Grok and Claude automatically load global rules + this repo-root file. Deeper `AGENTS.md` files win on conflicts.

---

## .dotfiles-Specific Notes

- When editing anything under `home/user/`, always test with a build first:
  - `nix build .#homeConfigurations.<your-host>.activationPackage`
  - Or the full `home-manager build --flake ~/.dotfiles#<user>`
- After approval, the user runs `home-manager switch --flake ~/.dotfiles#<user>`
- Changes to `flake.nix`, `home/user/packages.nix`, or overlays should be validated with `nix flake check` where possible.
- The `AGENTS.md` in this directory takes precedence only while the agent is operating inside the dotfiles tree.

For the complete harness rules (agent discipline, systematic debugging, verification-before-completion, writing-plans, etc.), see the global `~/.grok/AGENTS.md`.

---

**Single source of truth for the long content**: `home/config/grok/AGENTS.md` (linked by Home Manager in `home/user/config.nix`).
**Repo-root version**: Kept minimal to avoid duplication while still providing directory-specific guidance.
