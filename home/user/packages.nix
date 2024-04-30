{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs; [
    pkgs-unstable.dust
    bats
    nix-prefetch-git
    asdf-vm
    bat
    bat
    delta
    fd
    firefox
    fzf
    gh
    gnumake
    golangci-lint
    hclfmt
    kitty
    lsof
    markdownlint-cli
    neofetch
    pre-commit
    python3
    ripgrep
    unzip
    shellharden
    shfmt
    telegram-desktop
    tmux
    tmuxinator
    tree
    yarn
    zip
    zoxide
    wofi
    pcmanfm
    hyprpaper
    pywal
    pyprland
    pavucontrol
    grim
    slurp
    libappindicator-gtk3
    awscli2
    packer
    pavucontrol
    ruff
    tflint
    terraform
    terragrunt
    dive
    xdg-utils
    terraform-docs
    circleci-cli
    gopls
    solargraph
    nixd

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

    # unfree
    # For zoom
    zoom-us # sharing does not work with zoom-us :(
    glxinfo # For zoom
    pulseaudioFull # For zoom
    gsettings-desktop-schemas # For zoom

    slack
    discord
    spotify
    pkgs-unstable.steam

    libreoffice-qt

    # libs
    zlib

    # languages
    gcc
    go
    lua
    nodejs_21
    rustup

    catppuccin-cursors.macchiatoBlue
    catppuccin-gtk
    papirus-folders
  ];
}
