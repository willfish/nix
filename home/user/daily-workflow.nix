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
in
{
  home.packages = [ agenda ] ++ lib.optional isGraphicalLinux workflow;

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
