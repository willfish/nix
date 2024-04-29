
{ pkgs, pkgs-unstable, ... }:
{
  home.packages = with pkgs; [
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
    nodejs
    pre-commit
    python3
    ripgrep
    rust-analyzer
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
    wezterm
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
    zoom-us
    glxinfo
    pulseaudioFull

    slack
    discord
    spotify
    steam

    # libs
    zlib
  ];
}
