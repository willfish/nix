{ config, ... }:
{
  imports = [ ./base.nix ];

  documentation.enable = false;

  assertions = [
    {
      assertion =
        !config.services.displayManager.sddm.enable
        && !config.boot.plymouth.enable
        && !config.services.xserver.enable;
      message = "server role must remain headless";
    }
  ];
}
