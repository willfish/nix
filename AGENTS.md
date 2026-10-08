# .dotfiles

William's flake-based NixOS/Home Manager repo. Follow `home/config/llm/AGENTS.md` plus these rules. Prefer focused changes.

## Layout and hosts

- Andromeda: Thelio Major Threadripper desktop, main workstation.
- Foundation: Framework 13 AMD AI-300 Series laptop.
- Terminus: Beelink NAS/headless host.
- `flake.nix`: inputs, outputs, hosts; `system/`: per-host NixOS.
- `home/user/`: Home Manager; optional `modules/` for reusable modules. Packages in `packages.nix`, programs in `programs.nix`, shell config in `shells.nix` + `programs.nix`. Fish is primary.
- `programs/`: owned C/Rust program source, colocated `default.nix` packages and manual local checks. Keep deployment wiring in `home/user/` or `system/`.
- `home/config/`: static home/config symlinks. Hyprland settings live under `home/config/hyprland/`.
- `home/config/llm/`: canonical shared AGENTS, `skills/`, `guides/`, `process-skills/`, `references/`; deployed via Home Manager.
- `~/Repositories/nixpkgs` is ONLY for overlaid packages (e.g. variety). Pi uses the `llm-agents.nix` input. Local tools include mux.

## Development

The dev shell installs commit checks. Pushes do not build NixOS or Home Manager configurations. See `docs/nixos-host-operations.md` for manual checks, runtime tests, switches and recovery.

Use `hmswitch` for Home Manager activation. After module changes, build the selected host, activate and verify before committing, subject to authorization. Verify added packages exist in inputs/nixpkgs. Never commit secrets/private data.

Brave debugging is configured in programs.nix on port 9222; probe `/json/version`. Use browser MCP first, prefer evaluate_script to snapshots for extraction. For GitHub use MCP first, then gh rather than scraping.

Finish `~/.dotfiles` work before you stop: commit on master, run `hmswitch`, then push. Do this even when the session cwd is another repository. The same finish line is in `home/config/llm/AGENTS.md`, which every agent already has. Leaving the tree dirty, or waiting to be asked, is not done. Skip only when the user explicitly said not to commit, switch, or push.

NEVER USE BRANCHES UNLESS EXPLICITLY INSTRUCTED
