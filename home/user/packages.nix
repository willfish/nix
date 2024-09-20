{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
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

    # Nix tools
    nil
    nixd
    nix-prefetch-git

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

    direnv
    pwgen

    postgresql

    csvtool
    inetutils

    dig
    alacritty

    ansible
    lsd
    pandoc

    # fun stuff
    cowsay
    nyancat
    fortune
    lolcat
    neofetch
  ];
}
