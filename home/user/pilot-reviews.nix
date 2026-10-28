{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
{
  config =
    lib.mkIf
      (builtins.elem hostName [
        "andromeda"
        "foundation"
      ])
      {
        services.pilot-reviews = {
          enable = true;
          piExecutable = "${pkgs.pi-coding-agent}/bin/pi";
          environmentSettingsFile =
            if hostName == "andromeda" then
              "${config.home.homeDirectory}/.config/pilot-reviews/automatic-environments.json"
            else
              null;
          sshAuthSock = "${config.home.homeDirectory}/.config/herdr/herdr.sock.agent";
          # Enabled by default; the operator's saved off switch survives restart.
          autoStart = true;
          autoPublish = true;
          startPaused = false;
          initialReviewDispatch = true;
          notifications = true;
          concurrency = if hostName == "andromeda" then 5 else 1;
          tracked = true;
          menu = {
            enable = true;
            # Both desktops control one publisher and one persistent off switch.
            controllerHost = if hostName == "foundation" then "andromeda" else null;
            keybinding = "CTRL, H";
            browserCommand = [
              "${config.home.profileDirectory}/bin/brave"
              "--new-tab"
            ];
          };
        };
      };
}
