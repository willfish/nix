{ config, ... }:
{
  imports = [ ./base.nix ];

  documentation.enable = false;

  assertions = [
    {
      assertion =
        !config.services.desktopManager.cosmic.enable
        && !config.services.displayManager.cosmic-greeter.enable
        && !config.services.greetd.enable
        && !config.programs.regreet.enable
        && !config.services.xserver.enable;
      message = "server role must remain headless";
    }
  ];
}
