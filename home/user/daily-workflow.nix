{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  credentials =
    config.sops.secrets.GOOGLE_CALENDAR_ICAL.path
      or "${config.xdg.configHome}/sops-nix/secrets/GOOGLE_CALENDAR_ICAL";
  agendaPackage = import ../../programs/daily-agenda { inherit pkgs; };
  agenda = pkgs.writeShellApplication {
    name = "daily-agenda";
    text = ''
      export DAILY_CALENDAR_CREDENTIALS=''${DAILY_CALENDAR_CREDENTIALS:-${lib.escapeShellArg credentials}}
      exec ${agendaPackage}/bin/daily-agenda "$@"
    '';
  };
  workflowPackage = import ../../programs/daily-launcher { inherit pkgs; };
  workflow = pkgs.writeShellApplication {
    name = "daily-workflow";
    runtimeInputs = [
      pkgs.systemd
      pkgs.ghostty
      pkgs.fish
      agenda
    ];
    text = ''
      export PATH=${lib.escapeShellArg "${config.home.profileDirectory}/bin"}:"$PATH"
      exec ${workflowPackage}/bin/daily-workflow "$@"
    '';
  };
  recapPackage = import ../../programs/standup-recap { inherit pkgs; };
  # Pi and the MCP wrappers live in ~/.local/bin and the profile, which a
  # systemd user unit does not inherit.
  recap = pkgs.writeShellApplication {
    name = "standup-recap";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.curl
    ];
    text = ''
      export PATH="$HOME/.local/bin:${config.home.profileDirectory}/bin:$PATH:/run/current-system/sw/bin"
      exec ${recapPackage}/bin/standup-recap "$@"
    '';
  };
  recapEnabled = isGraphicalLinux && config.dotfiles.privateEnabled;
in
{
  home.packages = [
    agenda
  ]
  ++ lib.optional isGraphicalLinux workflow
  ++ lib.optional recapEnabled recap;

  xdg.configFile = lib.mkIf recapEnabled {
    "standup-recap/config".source = ../config/standup-recap/config;
    "standup-recap/prompt.md".source = ../config/standup-recap/prompt.md;
  };

  # Fills today's note from the previous working day. Startup and 09:00 both
  # trigger it; a completed marker makes the second trigger a no-op.
  systemd.user.services.standup-recap = lib.mkIf recapEnabled {
    Unit.Description = "Fill today's daily note with the previous working day's activity";
    Service = {
      Type = "oneshot";
      ExecStart = "${recap}/bin/standup-recap";
      TimeoutStartSec = 1200;
    };
  };
  systemd.user.timers.standup-recap = lib.mkIf recapEnabled {
    Unit.Description = "Fill the daily note at login and at 09:00 on weekdays";
    Timer = {
      OnStartupSec = "3m";
      OnCalendar = "Mon..Fri 09:00";
      Persistent = true;
    };
    Install.WantedBy = [ "timers.target" ];
  };

  # The launcher reads cached events; refresh runs separately on this timer.
  # A missing feed file leaves the timer inert.
  systemd.user.services.daily-agenda-refresh =
    lib.mkIf (isGraphicalLinux && config.dotfiles.privateEnabled)
      {
        Unit = {
          Description = "Refresh the private read-only daily calendar cache";
          ConditionPathExists = credentials;
        };
        Service = {
          Type = "oneshot";
          ExecStart = "${agenda}/bin/daily-agenda --refresh";
          UMask = "0077";
          StandardOutput = "null";
          StandardError = "null";
          TimeoutStartSec = 120;
        };
      };
  systemd.user.timers.daily-agenda-refresh =
    lib.mkIf (isGraphicalLinux && config.dotfiles.privateEnabled)
      {
        Unit.Description = "Refresh calendar events every fifteen minutes";
        Timer = {
          OnStartupSec = "2m";
          OnUnitActiveSec = "15m";
        };
        Install.WantedBy = [ "timers.target" ];
      };
}
