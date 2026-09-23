{
  config,
  pkgs,
  lib,
  readSopsSecret,
  hostName ? null,
  ...
}:
let
  inherit (pkgs) stdenv;
  capabilities = config.dotfiles.capabilities;
  isTerminus = capabilities.nas;
  skipsForteAndWalls = builtins.elem hostName [
    "foundation"
    "relay"
    "terminus"
  ];
  muxWithDefaultBackend = pkgs.writeShellScriptBin "mux" ''
    export MUX_BACKEND="''${MUX_BACKEND:-herdr}"
    exec ${pkgs.mux}/bin/mux "$@"
  '';

  # TOTP from sops-nix seeds (never print seeds). Used by agents for MFA fill.
  # Secret file: $SOPS_NIX_SECRETS_DIR/TOTP_<NAME>_SECRET  (base32 seed)
  # With no name, select an available account with fzf.
  # Example: totp-from-sops ACCOUNT  → 6-digit code on stdout only
  totpFromSops = pkgs.writeShellApplication {
    name = "totp-from-sops";
    runtimeInputs = [
      pkgs.oath-toolkit
      pkgs.coreutils
      pkgs.fzf
    ];
    text = ''
      set -euo pipefail

      secrets_dir="''${SOPS_NIX_SECRETS_DIR:-''${XDG_CONFIG_HOME:-$HOME/.config}/sops-nix/secrets}"
      if [ "$#" -lt 1 ]; then
        accounts=()
        for candidate in "$secrets_dir"/TOTP_*_SECRET; do
          [ -r "$candidate" ] || continue
          account="''${candidate##*/TOTP_}"
          accounts+=("''${account%_SECRET}")
        done
        if [ "''${#accounts[@]}" -eq 0 ]; then
          echo "totp-from-sops: no readable TOTP_*_SECRET files in $secrets_dir" >&2
          exit 66
        fi
        target="$(printf '%s\n' "''${accounts[@]}" | fzf --prompt='TOTP account> ')" || exit $?
        [ -n "$target" ] || exit 130
      else
        target="$1"
        shift
      fi

      digits=6
      period=30
      while [ "$#" -gt 0 ]; do
        case "$1" in
          --digits) digits="$2"; shift 2 ;;
          --period) period="$2"; shift 2 ;;
          *) echo "unknown arg: $1" >&2; exit 64 ;;
        esac
      done

      if [ -f "$target" ]; then
        secret_file="$target"
      elif [ -f "$secrets_dir/$target" ]; then
        secret_file="$secrets_dir/$target"
      elif [ -f "$secrets_dir/TOTP_''${target}_SECRET" ]; then
        secret_file="$secrets_dir/TOTP_''${target}_SECRET"
      else
        echo "totp-from-sops: secret not found for '$target'" >&2
        echo "expected: $secrets_dir/TOTP_''${target}_SECRET (or a readable file path)" >&2
        echo "import with: totp-sops-import $target" >&2
        exit 66
      fi

      if [ ! -r "$secret_file" ]; then
        echo "totp-from-sops: not readable: $secret_file" >&2
        exit 66
      fi

      seed="$(tr -d '[:space:]\"' <"$secret_file")"
      if [ -z "$seed" ]; then
        echo "totp-from-sops: empty seed in $secret_file" >&2
        exit 66
      fi

      # Pass the seed on stdin so it never appears in process arguments.
      printf '%s\n' "$seed" | oathtool --totp -b -d "$digits" -s "$period" -
    '';
  };

  # Import a base32 TOTP seed into the private configuration without printing it.
  # Commit/push there and update the nix-config input before switching.
  totpSopsImport = pkgs.writeShellApplication {
    name = "totp-sops-import";
    runtimeInputs = [
      pkgs.oath-toolkit
      pkgs.sops
      pkgs.jq
      pkgs.coreutils
    ];
    text = ''
      set -euo pipefail

      if [ "$#" -lt 1 ]; then
        echo "usage: totp-sops-import <NAME> [base32-seed-file|-]" >&2
        echo "  Stores seed as TOTP_<NAME>_SECRET in ~/Repositories/nix-config/secrets/env.yaml" >&2
        echo "  Seed via arg file, stdin (-), or interactive prompt (hidden)." >&2
        echo "  Never prints the seed. Validates with oathtool before writing." >&2
        exit 64
      fi

      name="$1"
      case "$name" in
        *[!A-Za-z0-9_]*|"") echo "NAME must be [A-Za-z0-9_]+" >&2; exit 64 ;;
      esac
      key="TOTP_''${name}_SECRET"
      src="''${2:-}"

      if [ -z "$src" ]; then
        if [ -t 0 ]; then
          echo -n "Paste base32 TOTP seed for $name (input hidden): " >&2
          stty -echo
          IFS= read -r seed
          stty echo
          echo >&2
        else
          seed="$(cat)"
        fi
      elif [ "$src" = "-" ]; then
        seed="$(cat)"
      else
        seed="$(cat -- "$src")"
      fi

      seed="$(printf '%s' "$seed" | tr -d '[:space:]\"')"
      if [ -z "$seed" ]; then
        echo "totp-sops-import: empty seed" >&2
        exit 66
      fi

      # Validate without printing seed: oathtool fails on bad base32
      if ! code="$(printf '%s\n' "$seed" | oathtool --totp -b - 2>/dev/null)"; then
        echo "totp-sops-import: seed rejected by oathtool (not valid base32 TOTP?)" >&2
        exit 65
      fi
      if ! printf '%s' "$code" | grep -Eq '^[0-9]{6,8}$'; then
        echo "totp-sops-import: unexpected oathtool output shape" >&2
        exit 65
      fi

      config_root="''${NIX_CONFIG_ROOT:-$HOME/Repositories/nix-config}"
      env_yaml="''${DOTFILES_SECRETS_ENV:-$config_root/secrets/env.yaml}"
      if [ ! -f "$env_yaml" ]; then
        echo "totp-sops-import: missing $env_yaml" >&2
        exit 66
      fi
      export SOPS_AGE_KEY_FILE="''${SOPS_AGE_KEY_FILE:-$HOME/.config/sops/age/keys.txt}"

      printf '%s' "$seed" | jq -Rs . | sops set --value-stdin "$env_yaml" "[\"$key\"]"
      # wipe shell var
      seed=""

      echo "totp-sops-import: stored $key in env.yaml (seed not shown)"
      echo "next:"
      echo "  1. Add $key to the appropriate group in $config_root/secret-groups.json"
      echo "  2. Commit and push the private configuration"
      echo "  3. In ~/.dotfiles: direnv exec . nix flake update nix-config, then build and hmswitch"
      echo "  4. totp-from-sops $name   # should print a 6-digit code"
      echo "  5. Confirm the code matches Authy/phone for that account, then drop Authy for it"
    '';
  };
in
{
  home.packages =
    with pkgs;
    [
      # Shared shell, Git, Nix maintenance and diagnostics.
      openssl # Cryptographic library for SSL/TLS
      bat # Cat clone with syntax highlighting and git integration
      curl # Data transfer tool with support for many protocols
      bats # Bash automated testing system
      delta # Git diff viewer with syntax highlighting
      duf # Disk usage utility with a user-friendly interface
      dust # Disk usage analyzer (alternative to du)
      fd # Fast, simple alternative to find
      findutils # GNU find, locate, xargs, etc.
      fzf # Fuzzy finder for command-line searches
      gawk # GNU awk
      gh # GitHub CLI for repository management
      git-ignore # Fetch .gitignore templates using the packaged gitignore.io client
      git-lfs # Large file support (filter configured in git.nix on all platforms)
      just # Project command runner used by the repository Justfile
      gnupg # OpenPGP encryption and signing tools
      jq # Command-line JSON processor
      readSopsSecret # Canonical reader for sops-nix secret files
      lsd # Modern ls alternative with icons
      lsof # Lists open files and their processes
      p7zip # 7-Zip file archiver (supports multiple formats)
      pwgen # Password generator
      ripgrep # Fast, recursive grep alternative
      age # File encryption tool used with sops-nix
      sops # Editor/encryption tool for SOPS-managed secrets
      ssh-to-age # Convert SSH Ed25519 keys to age recipients/identities
      tree # Displays directory tree structure
      unzip # Tool to extract ZIP archives
      yq # Command-line YAML processor (like jq for YAML)
      zip # Tool to create ZIP archives

      nh
      nix-prefetch-github
      nix-tree
      python3
      pre-commit
      pi-coding-agent # Pi terminal agent, available on every Home Manager host
      herdr
      muxWithDefaultBackend
      dig
      doggo
      inetutils
      nmap
      mtr
      btop
      htop
      fastfetch
    ]
    ++ lib.optionals capabilities.work [
      valkey
      postgresql
      pgcli
      awscli2
      ssm-session-manager-plugin
      terraform
      terraform-docs
      terragrunt
      tflint
      sniffy
      smailer
    ]
    ++ lib.optionals capabilities.desktop [ xdg-terminal-exec ]
    ++ lib.optionals capabilities.email [ himalaya ]
    ++ lib.optionals (capabilities.accounting || capabilities.work || capabilities.personal) [
      oath-toolkit
      totpFromSops
      totpSopsImport
    ]
    ++ lib.optionals capabilities.documents [
      # Document tools for workstation and accounting automation.
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

    ]
    ++ lib.optionals capabilities.development [
      openssl.dev
      pkg-config
      dive
      lazydocker
      # Project-specific dependencies should live in project dev shells.
      bootdev-cli # Boot.dev lesson/challenge CLI (local nixpkgs overlay while PR is open)
      gcc # GNU Compiler Collection
      go # Go compiler and tooling
      gnumake # GNU Make build automation tool
      makeWrapper # Nix utility to wrap executables with env vars
      nodejs_24 # JavaScript runtime used by frontend and diagram tooling
      uv # Python package runner used by MCP stdio servers

      markdownlint-cli # Linter for Markdown files
      ruby
    ]
    ++ lib.optionals capabilities.personal [
      # Audio tools
      ffmpeg # Audio/video conversion and inspection tools
      sox # Sound processing tool - used for Claude Code notification chimes

    ]
    ++ lib.optionals (capabilities.personal && !(stdenv.isLinux && capabilities.desktop)) [ mpv ]
    ++ lib.optionals (stdenv.isDarwin && capabilities.desktop) [
      aerospace # i3-like tiling window manager for macOS
      brave # Privacy-focused browser
    ]
    ++ lib.optionals stdenv.isDarwin [
      tailscale # WireGuard-based private networking
    ]
    ++ lib.optionals (stdenv.isDarwin && capabilities.localTerminal) [ ghostty-bin ]
    ++ lib.optionals (stdenv.isDarwin && (capabilities.desktop || capabilities.hermes)) [
      docker_29 # Docker client for talking to Colima or other Docker daemons
      docker-compose # Docker Compose CLI
      # forte omitted on Darwin: 1.1.0 checkPhase fails without libmpv.2.dylib.
      pango # Text layout/rendering tools used by graphics/document pipelines
    ]
    ++ lib.optionals stdenv.isLinux [
      netcat-openbsd # Darwin supplies /usr/bin/nc; its Nix package is broken.
      bandwhich # Terminal bandwidth utilization tool
      iftop # Real-time network bandwidth monitoring tool
      inxi # System information script
      isd # Interactive systemd journal browser
      nload # Network traffic and bandwidth monitor
      strace # System call tracer for debugging
      tshark # Network protocol analyzer (terminal version of Wireshark)
    ]
    ++ lib.optionals (stdenv.isLinux && isTerminus) [
      immich-go # Import Google Photos takeouts into Immich
    ]
    ++ lib.optionals (stdenv.isLinux && capabilities.desktop) [
      cliamp # Terminal music player
      evince # Omarchy's PDF reader
      nautilus # Omarchy's file manager; GVfs is provided by the desktop service
      sushi # Nautilus file previews
      (mpv.override { scripts = [ mpvScripts.mpris ]; })
      file-roller # Graphical archive manager for opening and extracting archives
      gimp # GNU Image Manipulation Program (the pinned package is Linux-only)
      cosmic-ext-tweaks
      ghostty # GPU-accelerated terminal emulator (system package on NixOS; set as default via TERMINAL + xdg-terminal-exec)
      discord # Team collaboration and messaging app
      dropbox # Cloud storage and file synchronization service
      element-desktop # Matrix messaging client
      libation # Audio player with a focus on music libraries
      libreoffice-qt-fresh # Office suite with Qt interface (docs, spreadsheets, etc.)
      loupe # Image viewer with previous/next navigation for images in the same directory
      pavucontrol # Graphical PulseAudio volume control
      python3Packages.huggingface-hub # Hugging Face CLI for model downloads
      qbittorrent # BitTorrent client with a user-friendly interface
      sherlock # Hunt down social media accounts by username across
      spotify # Music streaming application
      telegram-desktop # Desktop client for Telegram messaging
      vokoscreen-ng # Screen recording tool with audio support
      wl-clipboard # Wayland clipboard tools used by Herdr helpers on COSMIC
      wlopm # Wayland output power control used by the COSMIC display toggle shortcut
      xclip # Clipboard tool (macOS has native pbcopy/pbpaste)
    ]
    ++ lib.optionals (capabilities.work && capabilities.desktop) [ slack ]
    ++ lib.optionals (stdenv.isDarwin && capabilities.personal) [
      discord
      spotify
    ]
    # Source-built personal flakes. Keep them off Relay, Foundation, and
    # Terminus, where they are not needed.
    ++ lib.optionals (stdenv.isLinux && !skipsForteAndWalls) [
      forte
      walls # Personal wallpaper manager (Rust); provides walls + walls-tray binaries
    ];

  home.activation.fixDarwinBraveSignature = lib.mkIf (stdenv.isDarwin && capabilities.desktop) (
    lib.hm.dag.entryAfter [ "copyApps" ] ''
      brave_app="$HOME/Applications/Brave Browser.app"

      if [ -d "$brave_app" ]; then
        /usr/bin/xattr -cr "$brave_app" >/dev/null 2>&1 || true
        if ! /usr/bin/codesign --force --deep --sign - "$brave_app" >/dev/null 2>&1; then
          echo "Warning: failed to ad-hoc sign $brave_app" >&2
        fi
      fi
    ''
  );

}
