{ pkgs-unstable, ... }:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];

  networking.hostName = "andromeda";
  hardware.system76.enableAll = true;

  programs = {
    steam = {
      enable = true;
      remotePlay.openFirewall = true;
      dedicatedServer.openFirewall = true;
      localNetworkGameTransfers.openFirewall = true;
    };
  };

  systemd = {
    user.services.connectBluetoothSpeaker = {
      description = "Connect my BT speaker on user login";
      wantedBy = [ "default.target" ];
      after = [ "default.target" "suspend.target" "hibernate.target" "hybrid-sleep.target" "bluetooth.service" ];
      serviceConfig = {
        Type = "oneshot";
        ExecStart = "${pkgs-unstable.bluez}/bin/bluetoothctl connect AC:A9:B4:00:0E:21";
      };
    };

    extraConfig = ''
      DefaultTimeoutStopSec=10s
    '';
  };
}
