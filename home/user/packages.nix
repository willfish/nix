{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
    # Strong integration with home-manager
    pkgs.zoxide
    pkgs.tmux
    pkgs.tmuxinator

    # For zoom
    pkgs.zoom-us
    pulseaudioFull
    gsettings-desktop-schemas
    pamixer
    dconf

    # For work
    redis
    postgresql
    cloudflare-warp
    warpd

    xdg-utils

    # Desktop apps
    telegram-desktop
    pavucontrol
    libreoffice-qt
    slack
    discord
    spotify
    glxinfo
    vscode
    gimp
    steamcmd
    protontricks
    wine
    shotwell
    vokoscreen-ng

    # Nix tools
    nil
    nixd
    nix-prefetch-git
    direnv

    # Utilities
    tldr
    jq
    wget
    htop
    btop
    dust
    bat
    bats
    delta
    fd
    fzf
    gh
    lsof
    markdownlint-cli
    pre-commit
    ripgrep
    unzip
    p7zip
    zip
    strace
    shellharden
    shfmt
    tree
    yarn
    pcmanfm
    awscli2
    ssm-session-manager-plugin # enables ecs exec
    packer
    pavucontrol
    dive
    tflint
    terraform
    terragrunt
    terraform-docs
    circleci-cli
    serverless
    xclip
    usbutils
    sysz

    # Build tools
    bison
    flex
    fontforge
    makeWrapper
    pkg-config
    gnumake
    gcc
    libiconv
    autoconf
    automake
    libtool
    stdenv

    # libs
    libffi
    libpqxx
    libxml2
    libxslt
    libyaml
    openssl
    zlib
    zlib.dev
    zlib.out

    # languages and their tools
    nodejs_latest
    yarn
    (python311.withPackages (python-pkgs: [
      python-pkgs.black
      python-pkgs.git-revise
      python-pkgs.git-sweep
      python-pkgs.pip
      python-pkgs.requests
      python-pkgs.setuptools
      python-pkgs.wheel
    ]))
    gopls
    golangci-lint
    ruff
    hclfmt

    csvtool
    pwgen
    inetutils
    dig
    ansible
    lsd
    pandoc

    # fun stuff
    cowsay
    nyancat
    fortune
    lolcat
    neofetch
    gnome-mahjongg
    gnome-calendar

    # indeed
    kubectl

    solana-cli
  ];
}
