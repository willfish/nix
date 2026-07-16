{ ... }:
{
  # Private remote access to NixOS hosts without port-forwarding.
  #
  # After the first `nh os switch` on each host, enrol it once with
  # `sudo tailscale up`, then log the same account into personal devices.
  services.tailscale = {
    enable = true;
    openFirewall = true;
  };

  # The LAN, tailscale0, and Mullvad interfaces need loose reverse-path checks.
  networking.firewall.checkReversePath = "loose";
  networking.firewall.trustedInterfaces = [ "tailscale0" ];
}
