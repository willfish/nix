# dotfiles

NixOS configurations and Home Manager setup for multiple machines across Linux and macOS, managed as a single Nix flake. Everything from system-level services to shell aliases lives here, declaratively defined and reproducible.

## Architecture

The flake produces three NixOS system configurations and two Home Manager user configurations (Linux and macOS). All NixOS systems share a common base with host-specific overrides layered on top. The Home Manager configuration uses platform conditionals to work on both `x86_64-linux` and `aarch64-darwin`.

```mermaid
flowchart TD
    FLAKE[flake.nix]
    COMMON[common-configuration.nix]

    subgraph HOSTS [NixOS Hosts]
        ANDROMEDA[andromeda]
        STARFISH[starfish]
        FOUNDATION[foundation]
    end

    subgraph HM [Home Manager]
        HM_LINUX[william (Linux)]
        HM_DARWIN[william (macOS)]
    end

    FLAKE --> HOSTS
    FLAKE --> HM
    COMMON --> ANDROMEDA & STARFISH & FOUNDATION
    HM_LINUX -.-> HOSTS

    classDef box stroke:#6366f1,stroke-width:2px
    class FLAKE,COMMON,HOSTS,HM,ANDROMEDA,STARFISH,FOUNDATION,HM_LINUX,HM_DARWIN box
```

### Flake Inputs

| Input | Purpose |
|-------|---------|
| `nixpkgs-unstable` | Primary package source |
| `home-manager` | Declarative user environment |
| `nixos-hardware` | Hardware-specific modules (Framework laptop) |
| `pre-commit-hooks` | Git hook management |
| `sniffy` | AWS secrets scanner |
| `smailer` | S3 email viewer |
| `mux` | Tmux session manager |
| `trade-tariff-tools` | ECS task management CLI |

## Repository Structure

```mermaid
flowchart TD
    ROOT["~/.dotfiles"]

    subgraph SYSTEM [System Layer]
        COMMON[common-configuration.nix]
        ANDROMEDA[andromeda/]
        STARFISH[starfish/]
        FOUNDATION[foundation/]
    end

    subgraph HOME [Home Manager]
        subgraph MODULES [Nix Modules]
            PKGS[packages.nix]
            SHELLS[shells.nix]
            PROGRAMS[programs.nix]
        end
        subgraph CONFIGS [Dotfiles]
            NVIM[nvim]
            GHOSTTY[ghostty]
            COSMIC[cosmic]
            OTHER[bin, tmuxinator...]
        end
    end

    subgraph OVERLAYS [Overlays]
        TOOLS[sniffy, smailer, mux, claude-code...]
    end

    ROOT --> SYSTEM & HOME & OVERLAYS

    classDef box stroke:#6366f1,stroke-width:2px
    class ROOT,SYSTEM,HOME,OVERLAYS,MODULES,CONFIGS box
```

## Hosts

### andromeda - Desktop Workstation

System76 Thelio Major with AMD Threadripper. Runs NVIDIA beta drivers with open source kernel modules and modesetting. Steam is enabled with firewall rules for local game transfers.

### starfish - Laptop

Dell Precision 5750. Inherits everything from the common configuration with no host-specific overrides.

### foundation - Travel Laptop

Framework 13 AMD AI-300 Series. Uses the `nixos-hardware` module for Framework-specific hardware support (power management, firmware, etc).

### Shared System Configuration

All hosts share `common-configuration.nix` which provides:

- **Boot:** systemd-boot with EFI support
- **Network:** NetworkManager, Avahi mDNS, Mullvad VPN, OpenSSH (key-only)
- **Desktop:** COSMIC desktop and greeter, X11
- **Audio:** PipeWire with PulseAudio compatibility
- **Services:** CUPS printing, Bluetooth, OpenSearch, GnuPG agent with SSH
- **Virtualisation:** Docker with host.docker.internal resolution
- **Fonts:** JetBrains Mono, Nerd Fonts (JetBrains Mono, Ubuntu, Ubuntu Mono)
- **Nix:** Flakes enabled, weekly garbage collection, auto-optimise store
- **Locale:** en_GB.UTF-8, Europe/London timezone

## Home Manager

The user configuration is split into focused modules that are composed in `home/user/default.nix`.

### Packages

Over 100 packages organised by purpose. Linux-only packages (GUI apps, clipboard tools, system tracers) are conditionally included using `lib.optionals stdenv.isLinux`.

| Category | Packages | Platform |
|----------|----------|----------|
| **AI** | gemini-cli | All |
| **AI** | claude-code | Linux |

### AI / LLM Agent Harness

A major recent addition is a **single-source-of-truth LLM agent harness** designed so that switching between different terminal AI tools feels seamless.

- **Universal rules** live in `home/config/llm/AGENTS.md` and are deployed to:
  - `~/.grok/AGENTS.md`
  - `~/.claude/CLAUDE.md`
  - `~/.codex/AGENTS.md`
  - `~/.gemini/GEMINI.md`

- **Job-specific content** (Jira workflows for the AI project on `transformuk.atlassian.net`, PR conventions, writing voice, RSpec patterns, trade-tariff frontend testing, epic/story style, etc.) lives in `home/config/llm/guides/` and is made available across tools.

- Native skill wrappers exist for both Codex/Grok formats (and Gemini support is prepared).

This means personal and work-specific conventions are consistent no matter whether you are using Grok CLI, Claude Code, Codex, or Gemini CLI. The architecture is documented in `home/config/llm/gemini/README.md` and the various `SKILL.md` files.
| **Desktop** | Brave, Chrome, Spotify, Slack, Telegram, LibreOffice, Variety | Linux |
| **Dev Tools** | gh, delta, lazydocker, dive, fzf, ripgrep, fd, jq, yq, httpie | All |
| **Networking** | nmap, mtr, doggo | All |
| **Networking** | tshark, bandwhich, iftop, nload | Linux |
| **Languages** | Node.js, Python 3, Ruby (YJIT), Go, Terraform, Lua | All |
| **LSP Servers** | nil, lua-language-server, gopls, ccls, bash-language-server, marksman, typescript-language-server | All |
| **Monitoring** | btop, htop | All |
| **Databases** | PostgreSQL, Valkey, pgcli | All |
| **Custom** | sniffy, smailer, mux, ecs | All |

### Shell

Fish is the default shell with extensive configuration:

- **Abbreviations** for navigation (`cdr`, `cdn`), Rails (`be`, `bx`, `rc`), Terraform (`tf`, `tfi`, `tfa`, `tfp`), Git, and AWS
- **Functions** for fetching `.gitignore` templates and managing dated notes
- **Zoxide** integration for fast directory jumping
- **Environment:** Neovim as editor/pager, YJIT-enabled Ruby, AWS eu-west-2 defaults

### Git

Signed commits with GPG key `BC6DED9479D436F5`. Delta as the diff viewer with the GitHub theme. Histogram diff algorithm, zdiff3 merge conflicts, auto-stash on rebase, and a commit template with JIRA format guidance. LFS enabled.

### Tmux

Rose Pine Moon theme. Vi key bindings, vim-tmux-navigator for seamless pane switching with `Alt+hjkl`, sessionx for fuzzy session management, thumbs for URL capturing, and yank for clipboard integration. Status bar at top. Tmuxinator session definitions for work, fun, and dotfiles projects.

### Desktop (Linux only)

COSMIC desktop with autotiling, focus-follows-cursor, and active window hints. Panel on the left (XS) with workspaces and status applets. Six static workspaces with `Super+1-9` switching. All settings managed declaratively via Home Manager.

### Neovim

Single `init.lua` configuration. Leader key is comma. Key mappings include `jk` for escape, JIRA ticket insertion from branch names, and quickfix list toggling.

### Ghostty

JetBrains Mono font, Catppuccin Mocha theme, slight transparency, 10K line scrollback, and a collection of custom GLSL shaders.

## Overlays

Packages that need to diverge from nixpkgs-unstable are overlaid through the flake:

| Package | Source | Reason | Platform |
|---------|--------|--------|----------|
| `claude-code` | `overlays/claude-code/` | Pinned ahead of upstream | Linux |
| `sniffy`, `smailer`, `mux` | GitHub flake inputs | Personal tools | All |
| `ecs` | `trade-tariff-tools` flake input | ECS task executor | All |

## Custom Scripts

Located in `home/config/bin/` and added to `$PATH`:

| Script | Purpose |
|--------|---------|
| `notes` / `notes_on` | Fzf-based note browser and dated note creator with templates |
| `gcall` | Nix garbage collection for user and root stores |

## Usage

Rebuild a NixOS system:

```bash
sudo nixos-rebuild switch --flake .#andromeda   # desktop
sudo nixos-rebuild switch --flake .#starfish    # dell laptop
sudo nixos-rebuild switch --flake .#foundation  # framework laptop
```

Rebuild the Home Manager configuration:

```bash
home-manager switch --flake .#william          # Linux
home-manager switch --flake .#william-darwin   # macOS
```

Update all flake inputs:

```bash
nix flake update
```

## Pre-commit Hooks

Managed through `git-hooks.nix` and available in the dev shell (`nix develop`):

- **eclint** - EditorConfig validation
- **nil** - Nix language linting
- **ormolu** - Haskell formatting
- **end-of-file-fixer** - Ensure files end with a newline
- **trim-trailing-whitespace** - Clean up trailing spaces
