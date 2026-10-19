{
  config,
  lib,
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
          piExecutable = "${config.home.homeDirectory}/.local/bin/pi";
          sshAuthSock = "${config.home.homeDirectory}/.config/herdr/herdr.sock.agent";
          # Installing the unit never starts it. Manual starts and worker restarts
          # keep new reviews paused until explicitly enabled through the menu.
          startPaused = true;
          initialReviewDispatch = false;
          notifications = false;
          tracked = true;
          menu = {
            enable = true;
            keybinding = "CTRL, H";
            browserCommand = [
              "${config.home.profileDirectory}/bin/brave"
              "--new-tab"
            ];
          };
        };
      };
}
