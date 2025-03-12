{ pkgs-unstable, ... }:
let
  aliases = {
    a = "tmux attach";
    ag = "rg";
    la = "lsd -la";
    ll = "lsd -l";
    mux = "tmuxinator";
    tm = "tmux";
    v = "nvim";
    vi = "nvim";
    vim = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    a = "tmux attach";
    ag = "rg";

    book = "cd ~/Repositories/books";
    cdi = "cd ~/Repositories/indeed";
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

    mux = "tmuxinator";
    tm = "tmux";
  };
in
{
  programs.bash = {
    enable = true;
    shellAliases = aliases;
    initExtra = ''
    '';
  };

  programs.fish = {
    package = pkgs-unstable.fish;
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    plugins = [
      { name = "tide"; src = pkgs-unstable.fishPlugins.tide.src; }
    ];

    interactiveShellInit = ''
      set -gx AWS_DEFAULT_REGION eu-west-2
      set -gx AWS_REGION eu-west-2
      set -gx ERL_AFLAGS "-kernel shell_history enabled"
      set -gx PATH $HOME/.bin $PATH
      set -gx PATH $HOME/go/bin $PATH
      set -gx RUBYOPT --enable-yjit
      set -gx SAM_CLI_TELEMETRY 0
      set -gx fish_greeting ""
    '';

    functions = {
      gitignore = ''curl -sL https://www.gitignore.io/api/$argv'';

      today = ''notes_on (date +"%Y-%m-%d") today.md'';
      yesterday = ''notes_on (date -d yesterday +"%Y-%m-%d") today.md'';
      tomorrow = ''notes_on (date +%F -d "tomorrow") today.md'';

      logs = ''echo frontend_log\nbackend_log\nadmin_log\nduty_log | fzf | xargs -- echo'';
      frontend_log = ''log_for "https://www.trade-tariff.service.gov.uk/healthcheck" trade-tariff-frontend'';
      backend_log = ''log_for "https://www.trade-tariff.service.gov.uk/api/v2/healthcheck" trade-tariff-backend'';
      duty_log = ''log_for "https://www.trade-tariff.service.gov.uk/duty-calculator/healthcheck" trade-tariff-duty-calculator'';
      admin_log = ''log_for "https://admin.trade-tariff.service.gov.uk/healthcheck" trade-tariff-admin'';
      all_logs = ''
        frontend_log
        backend_log
        duty_log
        admin_log
      '';

    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.package = pkgs-unstable.zoxide;
}
