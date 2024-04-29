{ config, pkgs, pkgs-unstable, home-manager, ... }:

let
  aliases = {
    ag = "rg";
    cd = "z";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    ll = "ls -l";
    la = "ls -la";
    vim = "nvim";
    vi = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    ag = "rg";
    cd = "z";
    cdr = "cd ~/Repositories";
    hm = "cd ~/Repositories/hmrc";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    rc = "bundle exec rails console";
    rs = "bundle exec rails server";
    rr = "bundle exec rails routes --expanded";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";
    tg = "terragrunt";
    pbcopy = "wl-copy";
    pbpaste = "wl-paste";
    g = "git";
  };
in
{
  imports = [./user];

  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "23.11";

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

  home.sessionVariables = {
    VISUAL = "nvim";
    EDITOR = "nvim";
    ASDF_RUBY_BUILD_VERSION = "master";
    LESS = "-R";
    GIT_PAGER = "delta";
    MANPAGER = "nvim +Man!";
    PAGER = "less --raw-control-chars -F -X";
    RUBYOPT = "--enable-yjit";
    fish_greeting = "";
    XDG_CURRENT_DESKTOP = "gnome";
  };

  # Let Home Manager install and manage itself.
  programs.home-manager.enable = true;

  programs.bash = {
    enable = true;
    shellAliases = aliases;
    initExtra = ''
      source ${pkgs.asdf-vm}/share/asdf-vm/asdf.sh
    '';
  };

  programs.fish = {
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;
    shellInit = ''
      source ${pkgs.asdf-vm}/share/asdf-vm/asdf.fish
    '';
    plugins = with pkgs.fishermanPackages; [
      { name = "tide"; src = pkgs.fishPlugins.tide.src; }
    ];
  };

  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.enableBashIntegration = true;

  programs.fzf.enable = true;

  services.network-manager-applet.enable = true;
}
