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
  agendaPython = pkgs.python3.withPackages (ps: [
    ps.icalendar
    ps.recurring-ical-events
  ]);
  agenda = pkgs.writeShellApplication {
    name = "daily-agenda";
    runtimeInputs = [ agendaPython ];
    text = ''
      export DAILY_CALENDAR_CREDENTIALS=''${DAILY_CALENDAR_CREDENTIALS:-${lib.escapeShellArg credentials}}
      exec python3 ${../config/launcher/agenda.py} "$@"
    '';
  };
  workflow = pkgs.writeShellApplication {
    name = "daily-workflow";
    runtimeInputs = [
      pkgs.python3
      pkgs.systemd
      pkgs.ghostty
      pkgs.fish
      agenda
    ];
    text = ''
      export PATH=${lib.escapeShellArg "${config.home.profileDirectory}/bin"}:"$PATH"
      exec python3 ${../config/launcher/daily.py} "$@"
    '';
  };
in
{
  home.packages = [ agenda ] ++ lib.optional isGraphicalLinux workflow;

  # The launcher reads cached events; refresh runs separately on this timer.
  # A missing feed file leaves the timer inert.
  systemd.user.services.daily-agenda-refresh = lib.mkIf isGraphicalLinux {
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
  systemd.user.timers.daily-agenda-refresh = lib.mkIf isGraphicalLinux {
    Unit.Description = "Refresh calendar events every fifteen minutes";
    Timer = {
      OnStartupSec = "2m";
      OnUnitActiveSec = "15m";
    };
    Install.WantedBy = [ "timers.target" ];
  };
}
