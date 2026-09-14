# .dotfiles

William's flake-based NixOS/Home Manager repo. Follow `home/config/llm/AGENTS.md` plus these rules. Prefer focused changes.

## Layout and hosts

- Andromeda: Thelio Major Threadripper desktop, main workstation.
- Starfish: Dell Precision 5750 laptop.
- Foundation: Framework 13 AMD AI-300 Series laptop.
- Terminus: Beelink NAS/headless host.
- `flake.nix`: inputs, outputs, hosts; `system/`: per-host NixOS.
- `home/user/`: Home Manager; optional `modules/` for reusable modules. Packages in `packages.nix`, programs in `programs.nix`, shell config in `shells.nix` + `programs.nix`. Fish is primary.
- `home/config/`: static home/config symlinks, including declarative Cosmic DE settings.
- `home/config/llm/`: canonical shared AGENTS, `skills/`, `guides/`, `process-skills/`, `references/`; deployed via Home Manager.
- `~/Repositories/nixpkgs` is ONLY for overlaid packages (e.g. variety). Pi uses the `llm-agents.nix` input. Local tools include sniffy, smailer, mux and forte.

## Development

The dev shell installs commit checks and an affected-configuration pre-push gate; let the gate own push-time configuration checks rather than duplicating manual builds or broad flake checks. See `docs/nixos-host-operations.md` for selection rules, manual checks, runtime tests, switches and recovery.

Use `hmswitch` for Home Manager activation. After module changes, build the selected host, activate and verify before committing, subject to authorization; a push check does not replace deployment checks. Verify added packages exist in inputs/nixpkgs. Never commit secrets/private data.

Brave debugging is configured in programs.nix on port 9222; probe `/json/version`. Use browser MCP first, prefer evaluate_script to snapshots for extraction. For GitHub use MCP first, then gh rather than scraping.

Commit, switch and push home-manager changes when done.

NEVER USE BRANCHES UNLESS EXPLICITLY INSTRUCTED

ALWAYS FAST FORWARD MERGE BRANCHES INTO MASTER BEFORE PUSHING
