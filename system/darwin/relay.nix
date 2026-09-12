{ ... }:
{
  imports = [ ./headless.nix ];
  networking.hostName = "relay";
  nixpkgs.hostPlatform = "aarch64-darwin";
}
