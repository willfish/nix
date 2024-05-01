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
    steam-run
    image-roll

    # For zoom
    pkgs.zoom-us
    glxinfo
    pulseaudioFull
    gsettings-desktop-schemas

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
    nodejs
    rustup
    ruby_3_3
    python311.withPackages (ps: [ps.requests])
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
