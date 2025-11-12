{
  pkgs-unstable,
  sniffy,
  smailer,
  ...
}:
{
  home.packages = with pkgs-unstable; [
    # Services for work
    redis # In-memory data structure store (key-value database)
    postgresql # Open-source relational database system

    # Desktop apps
    audacity # Audio recording and editing software
    clementine # Music player with library management
    discord # Chat app for communities and gaming
    dropbox # Cloud storage and file synchronization service
    gimp # Image editing software (GNU Image Manipulation Program)
    libation # Audio player with a focus on music libraries
    libreoffice-qt # Office suite with Qt interface (docs, spreadsheets, etc.)
    obs-studio # Open Broadcaster Software for video recording and live streaming
    pavucontrol # Graphical PulseAudio volume control
    slack # Team collaboration and messaging app
    spotify # Music streaming application
    telegram-desktop # Desktop client for Telegram messaging
    variety # Wallpaper changer with customization options
    vokoscreen-ng # Screen recording tool with audio support
    xournalpp # Note-taking and PDF annotation tool

    # Utilities
    awscli2 # AWS command-line interface (version 2)
    bats # Bash automated testing system
    delta # Git diff viewer with syntax highlighting
    dive # Tool to explore Docker image layers
    dust # Disk usage analyzer (alternative to du)
    fd # Fast, simple alternative to find
    fzf # Fuzzy finder for command-line searches
    gemini-cli # Command-line client for the Gemini protocol
    gh # GitHub CLI for repository management
    htop # Interactive process viewer
    jq # Command-line JSON processor
    lazydocker # Terminal UI for Docker and Docker Compose
    lsof # Lists open files and their processes
    p7zip # 7-Zip file archiver (supports multiple formats)
    pwgen # Password generator
    ripgrep # Fast, recursive grep alternative
    ssm-session-manager-plugin # AWS plugin for SSM session management (e.g., ECS exec)
    strace # System call tracer for debugging
    sysz # Systemd service searcher (fzf-based)
    tree # Displays directory tree structure
    unzip # Tool to extract ZIP archives
    yq # Command-line YAML processor (like jq for YAML)
    zip # Tool to create ZIP archives

    # Build tools
    gcc # GNU Compiler Collection
    gnumake # GNU Make build automation tool
    makeWrapper # Nix utility to wrap executables with env vars
    stdenv # Standard environment for building packages in Nix

    # Networking tools
    dig # DNS lookup tool
    inetutils # Basic networking tools (e.g., ping, telnet)
    lsd # Modern ls alternative with icons

    # languages and their tools
    bash-language-server # Bash language server
    ccls # C/C++ language server
    gopls # Go language server for IDEs
    lua-language-server # Lua language server
    markdownlint-cli # Linter for Markdown files
    marksman # Markdown previewer with live reload
    nil # Nix language server for IDE integration
    nodejs_latest # Used just to install language servers
    pre-commit # Framework for managing pre-commit hooks
    python3
    stylua # Lua formatter
    ruby
    terraform # For terraform_fmt, terraform_validate
    terraform-docs
    terragrunt # For terragrunt-hclfmt
    tflint # For terraform_tflint
    typescript-language-server # Language server for typescript
    vscode-langservers-extracted # Extracted language servers for VSCode

    # fun stuff
    fastfetch # Highly customizable system information tool
    inxi # System information script

    dconf2nix # Convert dconf settings to Nix expressions - useful for gnome setups

    # My custom packages (usually go TUIs)
    sniffy.packages.${pkgs.system}.default
    smailer.packages.${pkgs.system}.default
  ];
}
