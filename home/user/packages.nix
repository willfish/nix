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
    steam
    steam-run
    image-roll

    # For zoom
    pkgs.zoom-us
    glxinfo
    pulseaudioFull
    gsettings-desktop-schemas
    pamixer

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
    wofi
    pcmanfm
    hyprpaper
    pywal
    pyprland
    grim
    slurp
    swappy
    libappindicator-gtk3
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
    # Wayland clipboard tooling
    # wl-clipboard
    # wl-clipboard-x11

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
    openssl
    libffi
    libxml2
    libxslt
    stdenv

    # libs
    zlib

    # languages and their tools
    gcc
    nodejs
    rustup
    ruby_3_2
    libpqxx
    libyaml
    (python311.withPackages (python-pkgs: [
      python-pkgs.beautifulsoup4
      python-pkgs.black
      python-pkgs.configobj
      python-pkgs.lxml
      python-pkgs.pillow
      python-pkgs.pip
      python-pkgs.pycairo
      python-pkgs.pygobject3
      python-pkgs.requests
      python-pkgs.setuptools
      python-pkgs.wheel
    ]))
    gopls
    solargraph
    ruff
    golangci-lint
    hclfmt
    nixd

    # themeing and fonts
    catppuccin-cursors.macchiatoBlue
    catppuccin-gtk
    papirus-folders

    cowsay
    direnv
    pwgen
  ];
}
