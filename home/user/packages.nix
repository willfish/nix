{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
    # Strong integration with home-manager so need to use pkgs
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

    xdg-utils

    # Desktop apps
    pavucontrol
    libreoffice-qt
    slack
    discord
    telegram-desktop
    spotify
    clementine # music player
    glxinfo # for checking if nvidia drivers are working
    gimp # image editor
    shotwell # photo manager
    vokoscreen-ng # screen recording
    xournalpp # note taking and annotating pdfs
    mozillavpn
    galculator
    qutebrowser
    simple-scan # scanner GUI
    variety # wallpaper changer
    audacity
    bitwarden # bitwarden cli

    # Nix tools
    nil
    nixd
    nix-prefetch-git
    nixpkgs-fmt

    # Utilities
    tldr
    jq
    yq
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
      python-pkgs.pip
      python-pkgs.requests
      python-pkgs.setuptools
      python-pkgs.wheel
    ]))
    gopls
    golangci-lint
    hclfmt
    ccls

    csvtool
    pwgen
    inetutils
    dig
    ansible
    lsd
    pandoc
    texlive.combined.scheme-medium

    # fun stuff
    cowsay
    nyancat
    fortune
    lolcat
    neofetch
    gnome-mahjongg

    # indeed
    kubectl

    solana-cli
    inxi
  ];
}
