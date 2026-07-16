{ ... }:
{
  # Terminus-only: private remote access to this server (no port-forwarding).
  # Clients are just the Tailscale app on phone/laptop — they do not need this module.
  #
  # After the first `nh os switch` on terminus, enrol it once with
  # `sudo tailscale up`, then log the same account into personal devices.
  services.tailscale = {
    enable = true;
    openFirewall = true;
  };

  # The LAN, tailscale0, and Mullvad interfaces need loose reverse-path checks.
  networking.firewall.checkReversePath = "loose";
  networking.firewall.trustedInterfaces = [ "tailscale0" ];
}
