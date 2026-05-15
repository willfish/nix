# dotfiles

NixOS configurations and Home Manager setup for multiple machines across Linux and macOS, managed as a single Nix flake. Everything from system-level services to shell aliases lives here, declaratively defined and reproducible.

## Architecture

The flake produces three NixOS system configurations and two Home Manager user configurations (Linux and macOS). All NixOS systems share a common base with host-specific overrides layered on top. The Home Manager configuration uses platform conditionals to work on both `x86_64-linux` and `aarch64-darwin`.

```mermaid
%%{init: {'theme': 'base', 'themeVariables': { 'fontSize': '14px', 'fontFamily': 'JetBrains Mono, monospace' }}}%%
flowchart TB
    %% Top-level shared nodes
    FLAKE[flake.nix]
    COMMON["<b>common-configuration.nix</b><br/>Shared system defaults"]

    %% NixOS host configurations
    subgraph HOSTS ["NixOS Configurations"]
        direction LR
        ANDROMEDA["<b>andromeda</b><br/><i>Thelio Major Threadripper</i><br/>NVIDIA GPU - Steam"]
        STARFISH["<b>starfish</b><br/><i>Dell Precision 5750</i><br/>Mobile workstation"]
        FOUNDATION["<b>foundation</b><br/><i>Framework 13 AMD AI-300</i><br/>NixOS Hardware module"]
    end

    %% Home Manager configurations
    subgraph HM ["Home Manager"]
        direction LR
        HM_LINUX["<b>william</b><br/><i>x86_64-linux</i>"]
        HM_DARWIN["<b>william-darwin</b><br/><i>aarch64-darwin</i>"]
    end

    %% Relationships
    FLAKE --> HOSTS
    FLAKE --> HM
    COMMON --> ANDROMEDA
    COMMON --> STARFISH
    COMMON --> FOUNDATION
    HM_LINUX -.->|deployed to NixOS hosts| HOSTS

    style FLAKE fill:#f0ecf9,stroke:#c4a7e7,stroke-width:2px,color:#3c3554
    style HOSTS fill:#e8f4f8,stroke:#9ccfd8,stroke-width:2px,color:#2d4a54
    style COMMON fill:#fdf0e0,stroke:#f6c177,stroke-width:2px,color:#5a4520
    style HM fill:#f0e0e8,stroke:#ea9a97,stroke-width:2px,color:#5a3040
    style HM_LINUX fill:#f8e8f0,stroke:#ea9a97,color:#5a3040
    style HM_DARWIN fill:#f8e8f0,stroke:#ea9a97,color:#5a3040
    style ANDROMEDA fill:#f8f4fc,stroke:#c4a7e7,color:#3c3554
    style STARFISH fill:#f8f4fc,stroke:#c4a7e7,color:#3c3554
    style FOUNDATION fill:#f8f4fc,stroke:#c4a7e7,color:#3c3554
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
%%{init: {'theme': 'base', 'themeVariables': { 'fontSize': '13px', 'fontFamily': 'JetBrains Mono, monospace' }}}%%
flowchart LR
    %% Top-level node
    ROOT["<b>~/.dotfiles</b>"]

    %% System layer
    subgraph SYSTEM_LAYER [" System Layer "]
        direction TB
        SYS_COMMON["common-configuration.nix<br/><i>Boot - Network - Audio<br/>Desktop - Services - Nix</i>"]
        SYS_ANDROMEDA["andromeda/<br/><i>NVIDIA - Steam</i>"]
        SYS_STARFISH["starfish/<br/><i>Defaults</i>"]
        SYS_FOUNDATION["foundation/<br/><i>Framework HW</i>"]
    end

    %% Home Manager layer
    subgraph HOME_LAYER [" Home Manager Layer "]
        direction TB

        subgraph MODULES [" Nix Modules "]
            direction TB
            MOD_PACKAGES["packages.nix<br/><i>100+ packages</i>"]
            MOD_SHELLS["shells.nix<br/><i>Fish - Bash - aliases</i>"]
            MOD_GIT["git.nix<br/><i>Delta - GPG signing</i>"]
            MOD_TMUX["tmux.nix<br/><i>Plugins - rose-pine</i>"]
            MOD_PROGRAMS["programs.nix<br/><i>Brave - direnv</i>"]
            MOD_ENVIRONMENT["environment.nix<br/><i>Editor - pager - vars</i>"]
            MOD_OTHER["email - config"]
        end

        subgraph CONFIGS [" Dotfile Configs "]
            direction TB
            CFG_NVIM["nvim/init.lua"]
            CFG_GHOSTTY["ghostty/config + shaders"]
            CFG_COSMIC["cosmic/ settings"]
            CFG_TMUXINATOR["tmuxinator/*.yml"]
            CFG_BIN["bin/ scripts"]
            CFG_OTHER["gitignore - gitmessage"]
        end
    end

    %% Overlays layer
    subgraph OVERLAYS_LAYER [" Overlays "]
        OVL_CLAUDE["claude-code"]
        OVL_TOOLS["sniffy - smailer<br/>mux - ecs"]
    end

    %% Relationships
    ROOT --> SYSTEM_LAYER
    ROOT --> HOME_LAYER
    ROOT --> OVERLAYS_LAYER

    style ROOT fill:#f0ecf9,stroke:#c4a7e7,stroke-width:2px,color:#3c3554
    style SYSTEM_LAYER fill:#e8f4f8,stroke:#9ccfd8,stroke-width:2px,color:#2d4a54
    style HOME_LAYER fill:#fdf0e0,stroke:#f6c177,stroke-width:2px,color:#5a4520
    style OVERLAYS_LAYER fill:#f0e0e8,stroke:#ea9a97,stroke-width:2px,color:#5a3040
    style MODULES fill:#fdf5e8,stroke:#f6c177,stroke-width:1px,color:#5a4520
    style CONFIGS fill:#fdf5e8,stroke:#f6c177,stroke-width:1px,color:#5a4520
    style SYS_COMMON fill:#f0f8fb,stroke:#9ccfd8,color:#2d4a54
    style SYS_ANDROMEDA fill:#f0f8fb,stroke:#9ccfd8,color:#2d4a54
    style SYS_STARFISH fill:#f0f8fb,stroke:#9ccfd8,color:#2d4a54
    style SYS_FOUNDATION fill:#f0f8fb,stroke:#9ccfd8,color:#2d4a54
    style MOD_PACKAGES fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_SHELLS fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_GIT fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_TMUX fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_PROGRAMS fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_ENVIRONMENT fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style MOD_OTHER fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_NVIM fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_GHOSTTY fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_COSMIC fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_TMUXINATOR fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_BIN fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style CFG_OTHER fill:#fef8f0,stroke:#f6c177,color:#5a4520
    style OVL_CLAUDE fill:#f8e8f0,stroke:#ea9a97,color:#5a3040
    style OVL_TOOLS fill:#f8e8f0,stroke:#ea9a97,color:#5a3040
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
