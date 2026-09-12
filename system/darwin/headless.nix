{ homeConfiguration, ... }:
{
  imports = [
    ./services.nix
    ./access.nix
    ./power.nix
    ./maintenance.nix
    ./monitoring.nix
    ./activation.nix
  ];
  # Determinate owns Nix, nix.conf and its maintenance policy.
  nix.enable = false;
  system.primaryUser = homeConfiguration.home.username;
  system.stateVersion = 6;
  assertions = [
    {
      assertion = homeConfiguration.dotfiles.darwinSystemServices;
      message = "The headless system and home must agree on service ownership.";
    }
  ];
}
