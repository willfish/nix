{ pkgs, pkgs-unstable, ... }:
let
  aliases = {
    ag = "rg";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    ll = "ls -l";
    la = "ls -la";
    vim = "nvim";
    vi = "nvim";
    vimdiff = "nvim -d";
    system-rebuild = "sudo nixos-rebuild switch --flake ~/.dotfiles/";
    home-rebuild = "home-manager switch --flake ~/.dotfiles/ -b backup";
    full-rebuild = "sudo nixos-rebuild switch --flake ~/.dotfiles/ && home-manager switch --flake ~/.dotfiles/ -b backup";
  };
  abbreviations = {
    ag = "rg";
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
  programs.bash = {
    enable = true;
    shellAliases = aliases;
  };

  programs.fish = {
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    plugins = [
      { name = "tide"; src = pkgs.fishPlugins.tide.src; }
    ];
  };

  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
}
