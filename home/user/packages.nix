{ pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
    # Strong integration with home-manager so need to use pkgs
    zoxide              # Fast directory jumper with smart history tracking
    tmux                # Terminal multiplexer for session management
    tmuxinator          # Tool to manage complex tmux sessions with YAML configs

    # For zoom
    zoom-us             # Video conferencing application
    pulseaudioFull      # Sound server with full features (e.g., Bluetooth support)
    gsettings-desktop-schemas  # GSettings schemas for desktop settings (e.g., Zoom UI)
    pamixer             # Command-line mixer for PulseAudio
    dconf               # Configuration database system for GNOME settings

    # For work
    redis               # In-memory data structure store (key-value database)
    postgresql          # Open-source relational database system

    xdg-utils           # Tools for desktop integration (e.g., opening files/URLs)

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
    markdownlint-cli    # Linter for Markdown files
    pre-commit          # Framework for managing pre-commit hooks
    ripgrep             # Fast, recursive grep alternative
    unzip               # Tool to extract ZIP archives
    p7zip               # 7-Zip file archiver (supports multiple formats)
    zip                 # Tool to create ZIP archives
    strace              # System call tracer for debugging
    tree                # Displays directory tree structure
    yarn                # Package manager for JavaScript
    pcmanfm             # Lightweight file manager
    awscli2             # AWS command-line interface (version 2)
    ssm-session-manager-plugin  # AWS plugin for SSM session management (e.g., ECS exec)
    packer              # Tool to create machine images (e.g., for AWS)
    dive                # Tool to explore Docker image layers
    tflint              # Terraform linter
    terraform           # Infrastructure as code tool
    terragrunt          # Wrapper for Terraform with better DRY practices
    terraform-docs      # Generates documentation for Terraform modules
    circleci-cli        # CircleCI command-line tool
    serverless          # Framework for building serverless applications
    xclip               # Command-line clipboard manager
    usbutils            # Tools to list and manage USB devices (e.g., lsusb)
    sysz                # Systemd service searcher (fzf-based)

    # Nix tools
    nil                 # Nix language server for IDE integration
    nix-prefetch-git    # Tool to prefetch Git repos for Nix expressions
    nixpkgs-fmt         # Formatter for Nix code

    # Build tools
    stdenv              # Standard environment for building packages in Nix
    makeWrapper         # Nix utility to wrap executables with env vars
    gnumake             # GNU Make build automation tool
    gcc                 # GNU Compiler Collection

    # libs
    libffi              # Foreign function interface library
    libiconv            # Library for character encoding conversion
    libpqxx             # C++ library for PostgreSQL
    libxml2             # XML parsing library
    libxslt             # XSLT processing library
    libyaml             # YAML parsing library
    openssl             # Cryptographic library for SSL/TLS
    zlib                # Compression library
    zlib.dev            # Development files for zlib (headers, etc.)
    zlib.out            # Runtime output for zlib

    # languages and their tools
    nodejs_latest       # Latest Node.js runtime for JavaScript
    yarn                # (Duplicate) JavaScript package manager
    (python311.withPackages (python-pkgs: [
      python-pkgs.black      # Python code formatter
      python-pkgs.pip        # Python package installer
      python-pkgs.requests   # HTTP library for Python
      python-pkgs.setuptools # Tools for packaging Python projects
      python-pkgs.wheel      # Python wheel file builder
    ]))
    gopls               # Go language server for IDEs
    golangci-lint       # Linter for Go code
    hclfmt              # Formatter for HashiCorp Configuration Language (HCL)
    ccls                # C/C++ language server

    csvtool             # Command-line tool for CSV manipulation
    pwgen               # Password generator
    inetutils           # Basic networking tools (e.g., ping, telnet)
    dig                 # DNS lookup tool
    ansible             # Automation tool for configuration management
    lsd                 # Modern ls alternative with icons
    pandoc              # Document converter (e.g., Markdown to PDF)
    texlive.combined.scheme-medium  # Medium-sized TeX Live distribution for LaTeX

    # fun stuff
    cowsay              # Generates ASCII art of a cow with text
    nyancat             # Animated Nyan Cat in the terminal
    fortune             # Displays random quotes or sayings
    lolcat              # Colors terminal output in rainbow style
    neofetch            # System info display with ASCII art
    gnome-mahjongg      # Mahjongg solitaire game

    # indeed
    kubectl             # Kubernetes command-line tool

    solana-cli          # CLI for Solana blockchain development
    inxi                # System information script
  ];
}
