{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs-unstable; [
    # Desktop apps
    pkgs.firefox
    telegram-desktop
    pavucontrol
    libreoffice-qt
    slack
    discord
    spotify
    steam
    image-roll

    # For zoom
    pkgs.zoom-us
    glxinfo
    pulseaudioFull
    gsettings-desktop-schemas


    # Utilities
    zoxide
    tldr
    jq
    wget
    htop
    btop
    dust
    bat
    bats
    nix-prefetch-git
    asdf-vm
    delta
    fd
    fzf
    zoxide
    gh
    lsof
    markdownlint-cli
    neofetch
    pre-commit
    ripgrep
    unzip
    zip
    strace
    shellharden
    shfmt
    tmux
    tmuxinator
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
    packer
    pavucontrol
    tflint
    terraform
    terragrunt
    dive
    xdg-utils
    terraform-docs
    circleci-cli

    # Wayland clipboard tooling
    wl-clipboard
    wl-clipboard-x11

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

    # libs
    zlib

    # languages and their tools
    gcc
    go
    lua
    nodejs
    rustup
    ruby
    python3
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
  ];
}
