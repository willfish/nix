{ config, ... }:
{
  networking.computerName = config.networking.hostName;
  networking.localHostName = config.networking.hostName;
  services.openssh = {
    enable = true;
    extraConfig = ''
      X11Forwarding no
    '';
  };
  # Keep existing authorized_keys, password policy and sudo authorization.
  # No SrvOS AuthorizedKeysFile=none or broad Nix trusted-users setting.
  security.pam.services.sudo_local.touchIdAuth = false;
  system.defaults.loginwindow = {
    GuestEnabled = false;
    autoLoginUser = null;
  };
}
