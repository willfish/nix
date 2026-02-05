{ pkgs, lib, ... }:
{
  home.packages = with pkgs; [
    # Services for work
    zoom-us
    valkey # Client for Valkey secure file sharing service
    postgresql # Open-source relational database system

    openssl # Cryptographic library for SSL/TLS
    openssl.dev # Development files for OpenSSL (headers, libs)
    pkg-config # Helper tool to manage library dependencies during compilation

    # Desktop apps
    clementine # Music player with library management
    dropbox # Cloud storage and file synchronization service
    libation # Audio player with a focus on music libraries
    libreoffice-qt # Office suite with Qt interface (docs, spreadsheets, etc.)
    pavucontrol # Graphical PulseAudio volume control
    qbittorrent # BitTorrent client with a user-friendly interface
    slack # Team collaboration and messaging app
    spotify # Music streaming application
    telegram-desktop # Desktop client for Telegram messaging
    variety # Wallpaper changer with customization options
    vokoscreen-ng # Screen recording tool with audio support

    # Email
    himalaya # CLI email client

    # Utilities
    awscli2 # AWS command-line interface (version 2)
    bat # Cat clone with syntax highlighting and git integration
    bats # Bash automated testing system
    delta # Git diff viewer with syntax highlighting
    dive # Tool to explore Docker image layers
    duf # Disk usage utility with a user-friendly interface
    dust # Disk usage analyzer (alternative to du)
    fd # Fast, simple alternative to find
    fzf # Fuzzy finder for command-line searches
    gh # GitHub CLI for repository management
    httpie # User-friendly command-line HTTP client
    isd # Interactive systemd journal browser
    jq # Command-line JSON processor
    lazydocker # Terminal UI for Docker and Docker Compose
    lsd # Modern ls alternative with icons
    lsof # Lists open files and their processes
    p7zip # 7-Zip file archiver (supports multiple formats)
    pgcli # PostgreSQL interactive terminal
    pwgen # Password generator
    ripgrep # Fast, recursive grep alternative
    ssm-session-manager-plugin # AWS plugin for SSM session management (e.g., ECS exec)
    strace # System call tracer for debugging
    tree # Displays directory tree structure
    unzip # Tool to extract ZIP archives
    yq # Command-line YAML processor (like jq for YAML)
    zip # Tool to create ZIP archives

    # Document tools
    (texlive.combine {
      inherit (texlive)
        scheme-small
        enumitem
        titlesec
        fancyhdr
        parskip
        booktabs
        tools
        collection-fontsrecommended
        hyperref
        xcolor
        ;
    }) # LaTeX distribution for PDF generation

    nh # Nix helper for nixos-rebuild and home-manager with nice diffs

    # Build tools
    gcc # GNU Compiler Collection
    gnumake # GNU Make build automation tool
    makeWrapper # Nix utility to wrap executables with env vars
    stdenv # Standard environment for building packages in Nix
    nix-tree # Visualize Nix derivation dependency trees
    nix-prefetch-github # Fetch GitHub repositories for Nix builds

    # Networking tools
    dig # DNS lookup tool
    doggo # DNS client (like dig)
    inetutils # Basic networking tools (e.g., ping, telnet)
    nmap # Network exploration and security auditing tool
    mtr # Network diagnostic tool (combines ping and traceroute)
    tshark # Network protocol analyzer (terminal version of Wireshark)

    # Monitoring tools
    bandwhich # Terminal bandwidth utilization tool
    btop # Resource monitor with a TUI
    htop # Interactive process viewer
    iftop # Real-time network bandwidth monitoring tool
    nload # Network traffic and bandwidth monitor

    # languages and their tools
    bash-language-server # Bash language server
    ccls # C/C++ language server
    dconf2nix # Convert dconf settings to Nix expressions - useful for gnome setups
    gopls # Go language server for IDEs
    lua-language-server # Lua language server
    markdownlint-cli # Linter for Markdown files
    marksman # Markdown previewer with live reload
    nil # Nix language server for IDE integration
    (lib.meta.lowPrio nodejs_latest) # Used just to install language servers
    pre-commit # Framework for managing pre-commit hooks
    black # Python code formatter
    isort # Python import sorter
    pyright # Python language server
    python3
    ruby
    stylua # Lua formatter
    terraform # For terraform_fmt, terraform_validate
    terraform-docs
    terragrunt # For terragrunt-hclfmt
    tflint # For terraform_tflint
    typescript-language-server # Language server for typescript
    vscode-langservers-extracted # Extracted language servers for VSCode

    # fun stuff
    fastfetch # Highly customizable system information tool
    inxi # System information script

    sniffy # Simple TUI for sniffing out unused secrets in AWS
    smailer # TUI for reviewing emails in an s3 bucket
    mux # Fast tmuxinator replacement in C

    cosmic-ext-tweaks

    # AI tools
    gemini-cli # Command-line client for the Gemini protocol
    claude-code # Command-line interface for Anthropic's Claude AI
  ];
}
