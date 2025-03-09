{ pkgs-unstable, ... }:
let
  aliases = {
    ag = "rg";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    ll = "lsd -l";
    la = "lsd -la";
    vim = "nvim";
    vi = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    ag = "rg";
    cdr = "cd ~/Repositories";
    book = "cd ~/Repositories/books";
    cdi = "cd ~/Repositories/indeed";
    hm = "cd ~/Repositories/hmrc";
    cdn = "cd ~/Notes";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    rc = "bundle exec rails console";
    rs = "bundle exec rails server";
    rr = "bundle exec rails routes --expanded";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";
    tg = "terragrunt";
    pbcopy = "xclip -selection clipboard";
    pbpaste = "xclip -selection clipboard -o";
    g = "git";
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
      gitignore = ''
        curl -sL https://www.gitignore.io/api/$argv
      '';
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

      patch_sorbet = ''
        set -l INTERPRETER ${pkgs-unstable.glibc}/lib/ld-linux-x86-64.so.2

        patch-sorbet $INTERPRETER
      '';

      home = ''
       set -g laptop_monitor "eDP-1"

       xrandr --output $laptop_monitor --auto
      '';

      office = ''
       set -g laptop_monitor "eDP-1"
       set -g external_monitor (xrandr | grep -v disconnected | grep connected | awk '{print $1}' | grep -v $laptop_monitor)

       xrandr --output $laptop_monitor --off --output $external_monitor --auto
      '';

      today = ''notes_on (date +"%Y-%m-%d") today.md'';
      yesterday = ''notes_on (date -d yesterday +"%Y-%m-%d") today.md'';
      tomorrow = ''notes_on (date +%F -d "tomorrow") today.md'';
    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.package = pkgs-unstable.zoxide;
}
