{
  pkgs,
  lib,
  ...
}:
let
  inherit (pkgs) stdenv;
in
{
  home.packages =
    with pkgs;
    [
      # Services for work
      valkey # Client for Valkey secure file sharing service
      postgresql # Open-source relational database system

      openssl # Cryptographic library for SSL/TLS
      openssl.dev # Development files for OpenSSL (headers, libs)
      pkg-config # Helper tool to manage library dependencies during compilation

      # Email
      himalaya # CLI email client

      # Utilities
      awscli2 # AWS command-line interface (version 2)
      bat # Cat clone with syntax highlighting and git integration
      curl # Data transfer tool with support for many protocols
      bats # Bash automated testing system
      delta # Git diff viewer with syntax highlighting
      dive # Tool to explore Docker image layers
      duf # Disk usage utility with a user-friendly interface
      dust # Disk usage analyzer (alternative to du)
      fd # Fast, simple alternative to find
      fzf # Fuzzy finder for command-line searches
      gh # GitHub CLI for repository management
      gnupg # OpenPGP encryption and signing tools
      httpie # User-friendly command-line HTTP client
      jq # Command-line JSON processor
      lazydocker # Terminal UI for Docker and Docker Compose
      lsd # Modern ls alternative with icons
      lsof # Lists open files and their processes
      p7zip # 7-Zip file archiver (supports multiple formats)
      pgcli # PostgreSQL interactive terminal
      pwgen # Password generator
      ripgrep # Fast, recursive grep alternative
      ssm-session-manager-plugin # AWS plugin for SSM session management (e.g., ECS exec)
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
      pandoc # Universal markup converter

      # Build tools
      gcc # GNU Compiler Collection
      go # Go compiler and tooling
      gnumake # GNU Make build automation tool
      makeWrapper # Nix utility to wrap executables with env vars
      nh # Nix helper for nixos-rebuild and home-manager with nice diffs
      nix-prefetch-github # Fetch GitHub repositories for Nix builds
      nix-tree # Visualize Nix derivation dependency trees
      nodejs_24 # JavaScript runtime used by frontend and diagram tooling

      # Networking tools
      dig # DNS lookup tool
      doggo # DNS client (like dig)
      inetutils # Basic networking tools (e.g., ping, telnet)
      nmap # Network exploration and security auditing tool
      mtr # Network diagnostic tool (combines ping and traceroute)

      # Monitoring tools
      btop # Resource monitor with a TUI
      htop # Interactive process viewer

      # Language runtimes and project tools
      markdownlint-cli # Linter for Markdown files
      pre-commit # Framework for managing pre-commit hooks
      python3
      ruby
      terraform # For terraform_fmt, terraform_validate
      terraform-docs
      terragrunt # For terragrunt-hclfmt
      tflint # For terraform_tflint

      # Audio tools
      ffmpeg # Audio/video conversion and inspection tools
      sox # Sound processing tool - used for Claude Code notification chimes

      # fun stuff
      fastfetch # Highly customizable system information tool

      antigravity # Google's Antigravity agentic IDE
      claude-code # Command-line interface for Anthropic's Claude AI
      codex # OpenAI Codex CLI coding agent
      gemini-cli # Google's Gemini CLI coding agent
      grok # Grok CLI from xAI
      sniffy # Simple TUI for sniffing out unused secrets in AWS
      smailer # TUI for reviewing emails in an s3 bucket
      mux # Fast tmuxinator replacement in C
      ecs # Interactive tool for running commands in ECS tasks
      bitwarden-desktop # Cross-platform password manager with a desktop client - enables fingerprint unlocking on macOS and Linux
    ]
    ++ lib.optionals stdenv.isDarwin [
      aerospace # i3-like tiling window manager for macOS
      brave # Privacy-focused browser
      docker # Docker client for talking to Colima or other Docker daemons
      docker-compose # Docker Compose CLI
      forte # Modern desktop music player with local library and streaming support
      ghostty-bin # GPU-accelerated terminal emulator
      llama-cpp # Local LLM inference tools
      pango # Text layout/rendering tools used by graphics/document pipelines
      slack # Team collaboration and messaging app
      spotify # Music streaming application
      tailscale # WireGuard-based private networking
    ]
    ++ lib.optionals stdenv.isLinux [
      bandwhich # Terminal bandwidth utilization tool
      cosmic-ext-tweaks
      dropbox # Cloud storage and file synchronization service
      forte # Modern desktop music player with local library and streaming support
      git-lfs # Large file support for model repos when needed
      iftop # Real-time network bandwidth monitoring tool
      inxi # System information script
      isd # Interactive systemd journal browser
      libation # Audio player with a focus on music libraries
      libreoffice-qt-fresh # Office suite with Qt interface (docs, spreadsheets, etc.)
      nload # Network traffic and bandwidth monitor
      pavucontrol # Graphical PulseAudio volume control
      python3Packages.huggingface-hub # Hugging Face CLI for model downloads
      qbittorrent # BitTorrent client with a user-friendly interface
      sherlock # Hunt down social media accounts by username across
      slack # Team collaboration and messaging app
      spotify # Music streaming application
      strace # System call tracer for debugging
      telegram-desktop # Desktop client for Telegram messaging
      tshark # Network protocol analyzer (terminal version of Wireshark)
      variety # Wallpaper changer with customization options
      vokoscreen-ng # Screen recording tool with audio support
      xclip # Clipboard tool (macOS has native pbcopy/pbpaste)
      zoom-us
    ];

}
