{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  graphicalSessionPath = lib.concatStringsSep ":" [
    "${config.home.homeDirectory}/.local/bin"
    "${config.home.homeDirectory}/.bin"
    "${config.home.homeDirectory}/go/bin"
    "${config.home.profileDirectory}/bin"
    "/etc/profiles/per-user/${config.home.username}/bin"
    "/nix/var/nix/profiles/default/bin"
    "/run/current-system/sw/bin"
  ];
in
{
  home.sessionVariables = {
    EDITOR = "nvim";
    GIT_PAGER = "delta";
    LESS = "-R";
    MANPAGER = "nvim +Man!";
    MUX_BACKEND = "herdr";
    NH_HOME_FLAKE = "${config.home.homeDirectory}/.dotfiles";
    NIXPKGS_ALLOW_UNFREE = 1;
    PAGER = "less --raw-control-chars -F -X";
    RUBYOPT = "--enable-yjit";
    VISUAL = "nvim";
    fish_greeting = "";
  }
  //
    lib.optionalAttrs
      (isGraphicalLinux || (pkgs.stdenv.isDarwin && config.dotfiles.capabilities.desktop))
      {
        BROWSER = "brave";
        DEFAULT_BROWSER = "brave";
      }
  // lib.optionalAttrs config.dotfiles.capabilities.localTerminal {
    TERMINAL = "ghostty";
  };

  home.sessionPath = [
    "$HOME/.bin"
    "$HOME/go/bin"
  ];

  systemd.user.sessionVariables = lib.mkIf isGraphicalLinux {
    PATH = graphicalSessionPath;
    SHELL = "/run/current-system/sw/bin/fish";
  };

  home.activation.importGraphicalSessionEnvironment = lib.mkIf isGraphicalLinux (
    lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      systemdStatus=$(${pkgs.systemd}/bin/systemctl --user is-system-running 2>&1 || true)

      if [[ $systemdStatus == 'running' || $systemdStatus == 'degraded' ]]; then
        ${pkgs.systemd}/bin/systemctl --user set-environment \
          PATH=${lib.escapeShellArg graphicalSessionPath} \
          SHELL=/run/current-system/sw/bin/fish

        env \
          PATH=${lib.escapeShellArg graphicalSessionPath} \
          SHELL=/run/current-system/sw/bin/fish \
          ${pkgs.dbus}/bin/dbus-update-activation-environment --systemd PATH SHELL
      else
        echo "Skipping graphical session env import: user systemd not running ($systemdStatus)."
      fi

      unset systemdStatus
    ''
  );
}
