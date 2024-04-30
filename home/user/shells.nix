{ pkgs, pkgs-unstable, ... }:
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
    system-rebuild = "sudo nixos-rebuild switch --flake ~/.dotfiles/";
    home-rebuild = "home-manager switch --flake ~/.dotfiles/ -b backup";
    full-rebuild = "sudo nixos-rebuild switch --flake ~/.dotfiles/ && home-manager switch --flake ~/.dotfiles/ -b backup";
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
      source ${pkgs-unstable.asdf-vm}/share/asdf-vm/asdf.fish
      source ${pkgs-unstable.fzf}/share/fish/vendor_functions.d/fzf_key_bindings.fish
      source ${pkgs-unstable.fzf}/share/fish/vendor_conf.d/load-fzf-key-bindings.fish
      source ${pkgs-unstable.fzf}/share/fzf/key-bindings.fish
      source ${pkgs-unstable.zoxide}/share/fish/vendor_completions.d/zoxide.fish
    '';
    plugins = with pkgs.fishermanPackages; [
      { name = "tide"; src = pkgs.fishPlugins.tide.src; }
    ];
  };
}
