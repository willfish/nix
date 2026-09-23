{ config, ... }:
{
  imports = [ ./base.nix ];

  documentation.enable = false;

  assertions = [
    {
      assertion =
        !config.services.greetd.enable
        && !config.programs.regreet.enable
        && !config.services.xserver.enable;
      message = "server role must remain headless";
    }
  ];
}
