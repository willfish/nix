{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
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

    # For zoom
    pkgs.zoom-us
    pulseaudioFull
    gsettings-desktop-schemas
    pamixer

    dconf

    # Strong integration with home-manager so need to work out using unstable pkgs
    pkgs.zoxide
    pkgs.tmux
    pkgs.tmuxinator

    # Utilities
    tldr
    jq
    wget
    htop
    btop
    dust
    bat
    bats
    nix-prefetch-git
    delta
    fd
    fzf
    gh
    lsof
    markdownlint-cli
    neofetch
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
    tflint
    terraform
    terragrunt
    dive
    xdg-utils
    terraform-docs
    circleci-cli
    serverless
    xclip

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
    zlib
    zlib.dev
    zlib.out
    libffi
    libxml2
    libxslt
    openssl
    libyaml
    libpqxx
    libyaml

    # languages and their tools
    nodejs
    yarn
    rustup
    (python311.withPackages (python-pkgs: [
      python-pkgs.black
      python-pkgs.requests
      python-pkgs.setuptools
      python-pkgs.wheel
      python-pkgs.pip
    ]))
    gopls
    golangci-lint
    ruff
    hclfmt
    nixd

    cowsay
    direnv
    pwgen

    postgresql

    csvtool
    inetutils

    dig
    alacritty

    ansible
    nyancat
    lsd
    pandoc
  ];
}
