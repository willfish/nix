{ pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
    zoom-us             # Video conferencing application

    # Services for work
    redis               # In-memory data structure store (key-value database)
    postgresql          # Open-source relational database system

    # Desktop apps
    pavucontrol         # Graphical PulseAudio volume control
    libreoffice-qt      # Office suite with Qt interface (docs, spreadsheets, etc.)
    slack               # Team collaboration and messaging app
    discord             # Chat app for communities and gaming
    telegram-desktop    # Desktop client for Telegram messaging
    spotify             # Music streaming application
    clementine          # Music player with library management
    gimp                # Image editing software (GNU Image Manipulation Program)
    vokoscreen-ng       # Screen recording tool with audio support
    xournalpp           # Note-taking and PDF annotation tool
    mozillavpn          # Mozilla’s VPN client for secure browsing
    simple-scan         # Simple GUI for scanning documents
    variety             # Wallpaper changer with customization options
    audacity            # Audio recording and editing software

    # Utilities
    jq                  # Command-line JSON processor
    yq                  # Command-line YAML processor (like jq for YAML)
    htop                # Interactive process viewer
    dust                # Disk usage analyzer (alternative to du)
    bats                # Bash automated testing system
    delta               # Git diff viewer with syntax highlighting
    fd                  # Fast, simple alternative to find
    fzf                 # Fuzzy finder for command-line searches
    gh                  # GitHub CLI for repository management
    lsof                # Lists open files and their processes
    ripgrep             # Fast, recursive grep alternative
    unzip               # Tool to extract ZIP archives
    p7zip               # 7-Zip file archiver (supports multiple formats)
    zip                 # Tool to create ZIP archives
    strace              # System call tracer for debugging
    tree                # Displays directory tree structure
    awscli2             # AWS command-line interface (version 2)
    ssm-session-manager-plugin  # AWS plugin for SSM session management (e.g., ECS exec)
    dive                # Tool to explore Docker image layers
    sysz                # Systemd service searcher (fzf-based)
    pwgen               # Password generator

    # Build tools
    stdenv              # Standard environment for building packages in Nix
    makeWrapper         # Nix utility to wrap executables with env vars
    gnumake             # GNU Make build automation tool
    gcc                 # GNU Compiler Collection

    # Networking tools
    inetutils           # Basic networking tools (e.g., ping, telnet)
    dig                 # DNS lookup tool
    lsd                 # Modern ls alternative with icons

    # languages and their tools
    nil                 # Nix language server for IDE integration
    markdownlint-cli    # Linter for Markdown files
    marksman            # Markdown previewer with live reload
    pre-commit          # Framework for managing pre-commit hooks
    gopls               # Go language server for IDEs
    ccls                # C/C++ language server
    nodejs_latest       # Used just to install language servers
    typescript-language-server # Language server for typescript
    bash-language-server # Bash language server
    stylua              # Lua formatter
    lua-language-server # Lua language server
    vscode-langservers-extracted # Extracted language servers for VSCode
    terraform           # For terraform_fmt, terraform_validate
    terragrunt          # For terragrunt-hclfmt
    tflint              # For terraform_tflint
    terraform-docs      # For terraform_docs

    # fun stuff
    neofetch            # System info display with ASCII art
    inxi                # System information script
    gnome-mahjongg      # Mahjongg solitaire game
    helix               # Post modern text editor
    zed-editor          # Modern text editor
  ];
}
