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
    '';
  };

  programs.fish = {
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    plugins = [
      { name = "tide"; src = pkgs.fishPlugins.tide.src; }
    ];

    interactiveShellInit = ''
      set -gx fish_greeting ""
      set -gx ERL_AFLAGS "-kernel shell_history enabled"
      set -gx SAM_CLI_TELEMETRY 0
      set -gx RUBYOPT --enable-yjit
    '';

    functions = {
      gitignore = ''
        curl -sL https://www.gitignore.io/api/$argv
      '';
      box = ''
        set -l environment (echo -e "development\nstaging\nproduction" | fzf)
        set -l key
        set -l host

        set -l environment (echo $environment | tr -d '\n')

        if test $environment = development
            echo "development environment"
            set host "18.171.95.123"
            set key '~/Downloads/restore-development.pem'
        else if test $environment = staging
            echo "staging environment"
            set host "ec2-13-42-116-249.eu-west-2.compute.amazonaws.com"
            set key '~/Downloads/jumpbox.pem'
        else if test $environment = production
            echo "production environment"
            set host "ec2-3-8-173-245.eu-west-2.compute.amazonaws.com"
            set key '~/Downloads/jumpbox-prod.pem'
        else
            echo "Unknown environment $environment"
        end

        echo "Connecting to $host"
        echo "ssh -i $key ec2-user@$host"
        ssh -i $key ec2-user@$host
      '';
      install_notes = ''
        if not test -e "$HOME/Notes/"
          git clone git@github.com:willfish/notes.git "$HOME/Notes"
        end
      '';
      notes_on = ''
        set -l on_date $argv[1]
        set -l notes_file $argv[2]
        set -l notes_directory "$HOME/Notes/$on_date"
        set -l fully_qualified_notes_file "$notes_directory/$notes_file"
        set -l template_file "$HOME/Notes/templates/$notes_file"

        install_notes

        if not test -e $fully_qualified_notes_file
            mkdir -p $notes_directory
            cp $template_file $fully_qualified_notes_file

            sed -i "s/TodaysDate/$on_date/" $fully_qualified_notes_file
        end

        pushd $notes_directory
        nvim $fully_qualified_notes_file
        popd
      '';
      today = ''notes_on (date +"%Y-%m-%d") today.md'';
      yesterday = ''notes_on (date -d yesterday +"%Y-%m-%d") today.md'';
      tomorrow = ''notes_on (date +%F -d "tomorrow") today.md'';
      log_for = ''
        set -l previous (pwd)
        set -l url $argv[1]
        set -l repo $argv[2]
        set -l sha1 (curl --silent $url | jq '.git_sha1' | tr -d '"')

        cd ~/Repositories/hmrc
        mkdir -p release-notes
        cd release-notes
        rm -rf $repo
        git clone --quiet https://github.com/trade-tariff/$repo.git
        cd $repo

        echo "*$repo*"
        echo
        echo "_"$sha1"_"
        echo
        git --no-pager log --merges HEAD...$sha1 --format="format:- %b" --grep 'Merge pull request'
        echo
        echo

        cd $previous
      '';
      frontend_log = ''log_for "https://www.trade-tariff.service.gov.uk/healthcheck" trade-tariff-frontend'';
      backend_log = ''log_for "https://www.trade-tariff.service.gov.uk/api/v2/healthcheck" trade-tariff-backend'';
      duty_log = ''log_for "https://www.trade-tariff.service.gov.uk/duty-calculator/healthcheck" trade-tariff-duty-calculator'';
      admin_log = ''log_for "https://admin.trade-tariff.service.gov.uk/healthcheck" trade-tariff-admin'';
      search_query_log = ''log_for "https://www.trade-tariff.service.gov.uk/api/search/healthcheck" trade-tariff-search-query-parser'';
      all_logs = ''
        frontend_log
        backend_log
        duty_log
        admin_log
        search_query_log
      '';
    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
}
