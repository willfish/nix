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
    package = pkgs-unstable.fish;
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    # plugins = [
    #   {
    #     name = "tide";
    #     src = pkgs-unstable.fishPlugins.tide.src;
    #   }
    # ];

    interactiveShellInit = ''
      set -gx AWS_DEFAULT_REGION eu-west-2
      set -gx AWS_REGION eu-west-2
      set -gx ERL_AFLAGS "-kernel shell_history enabled"
      set -gx PATH $HOME/.bin $PATH
      set -gx PATH $HOME/go/bin $PATH
      set -gx PATH /usr/local/bin $PATH
      set -gx RUBYOPT --enable-yjit
      set -gx SAM_CLI_TELEMETRY 0
      set -gx fish_greeting ""
    '';

    functions = {
      gitignore = ''curl -sL https://www.gitignore.io/api/$argv'';
      spy = ''
        set -l processes (ps aux | tail -n +2)

        # If no processes, exit
        if not count $processes > /dev/null
            echo "No processes found."
            return 1
        end

        # Use fzf to fuzzy-select a line
        set -l selected_line (printf '%s\n' $processes | fzf --height=40% --border --prompt="Select process: ")

        # If nothing selected (Esc/Ctrl+C), exit
        if test -z "$selected_line"
            echo "No process selected."
            return 1
        end

        # Extract PID (2nd column)
        set -l pid (echo $selected_line | awk '{print $2}')

        # Validate PID
        if not string match -qr '^[0-9]+$' -- $pid
            echo "Invalid PID: $pid"
            return 1
        end

        # Confirm process is running
        if not kill -0 $pid 2>/dev/null
            echo "Process $pid is not running."
            return 1
        end

        # Run strace with common useful syscalls
        echo "Attaching strace to PID $pid (Ctrl+C to stop)..."
        sudo strace -p $pid -e trace=openat,read,write,connect,accept,sendto,recvfrom,execve -q
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
