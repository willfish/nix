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
      description = "Reconnect Bluetooth Speaker After Resume";
      after = [ "graphical.target" "sleep.target" "suspend.target" "hibernate.target" "hybrid-sleep.target" ];
      serviceConfig = {
        Type = "oneshot";
        ExecStartPre = "${pkgs-unstable.coreutils}/bin/sleep 5";
        ExecStart = "${pkgs-unstable.bluez}/bin/bluetoothctl connect AC:A9:B4:00:0E:21";
        # Add resilience
        Restart = "on-failure";
        RestartSec = "5s";
        TimeoutStartSec = "15s";
      };
    };

    extraConfig = ''
      DefaultTimeoutStopSec=10s
    '';
  };
}
