{ pkgs, ... }:
let
  aliases = {
    a = "tmux attach";
    ag = "rg";
    la = "lsd -la";
    ll = "lsd -l";
    psql = "pgcli";
    tm = "tmux";
    v = "nvim";
    vi = "nvim";
    vim = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    a = "tmux attach";
    ag = "rg";

    cdn = "cd ~/Notes";
    cdr = "cd ~/Repositories";
    hm = "cd ~/Repositories/hmrc";

    pbcopy = "xclip -selection clipboard";
    pbpaste = "xclip -selection clipboard -o";

    rc = "bundle exec rails console";
    rr = "bundle exec rails routes --expanded";
    rs = "bundle exec rails server";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";

    tg = "terragrunt";

    tm = "tmux";

    prod = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/production";
    stag = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/staging";
    dev = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/development";
  };
in
{
  programs.bash = {
    enable = true;
    shellAliases = aliases;
  };

  programs.fish = {
    package = pkgs.fish;
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    interactiveShellInit = ''
      set -gx AWS_DEFAULT_REGION eu-west-2
      set -gx AWS_REGION eu-west-2
      set -gx ERL_AFLAGS "-kernel shell_history enabled"
      set -gx PATH $HOME/.bin $PATH
      set -gx PATH $HOME/go/bin $PATH
      set -gx PATH /usr/local/bin $PATH
      set -gx PATH ~/.config/emacs/bin $PATH
      set -gx RUBYOPT --enable-yjit
      set -gx SAM_CLI_TELEMETRY 0
      set -gx fish_greeting ""
    '';

    functions = {
      gitignore = ''curl -sL https://www.gitignore.io/api/$argv'';
      today = ''notes_on (date +"%Y-%m-%d") today.md'';
      yesterday = ''notes_on (date -d yesterday +"%Y-%m-%d") today.md'';
      tomorrow = ''notes_on (date +%F -d "tomorrow") today.md'';
    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.package = pkgs.zoxide;
}
