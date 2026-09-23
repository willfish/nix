{
  config,
  lib,
  pkgs,
  ...
}:
{
  imports = [
    ./base.nix
    ./hyprland.nix
    ./greeter.nix
    ./plymouth.nix
  ];

  boot.kernelParams = [ "btiso.enable=1" ];

  networking.extraHosts = ''
    127.0.0.1 host.docker.internal
  '';

  # MT7925 / NM create a wifi-p2p device that flaps disconnected and logs
  # IPv4 forwarding errors. It is unused here and was waking the Wi-Fi
  # reconnect helper on every state change.
  networking.networkmanager.unmanaged = [ "interface-name:p2p-dev-*" ];

  services.printing.enable = true;
  services.mullvad-vpn = {
    enable = true;
    package = pkgs.mullvad-vpn;
  };
  services.avahi = {
    enable = true;
    nssmdns4 = true;
    openFirewall = true;
  };

  services.pulseaudio.enable = false;
  security.rtkit.enable = true;
  services.pipewire = {
    enable = true;
    alsa.enable = true;
    alsa.support32Bit = true;
    pulse.enable = true;
    wireplumber.enable = true;
  };

  users.users.william = {
    extraGroups = [
      "audio"
      "bluetooth"
      "docker"
      "input"
      "libvirt"
      "networkmanager"
      "video"
    ];
    packages = with pkgs; [
      docker_29
      docker-compose
    ];
  };

  hardware.bluetooth = {
    enable = true;
    powerOnBoot = true;
    settings.General.Experimental = true;
  };

  environment.systemPackages = with pkgs; [
    ghostty
    xclip
  ];

  virtualisation.docker = {
    enable = true;
    package = pkgs.docker_29;
  };

  services.spice-vdagentd.enable = true;
  # acpid aborts after netlink ENOBUFS during input hotplug storms.
  # NixOS ships the unit without Restart=, leaving the system degraded.
  services.acpid.enable = true;
  systemd.services.acpid.serviceConfig = {
    Restart = "on-failure";
    RestartSec = "5s";
  };
  services.upower.enable = true;
  services.gvfs.enable = true;
  services.gnome.gnome-keyring.enable = true;
  security.polkit.enable = true;
  services.power-profiles-daemon.enable = lib.mkDefault (
    !config.hardware.system76.power-daemon.enable
  );
  services.xserver = {
    enable = true;
    xkb.layout = "us";
    xkb.variant = "";
  };

  fonts.packages = with pkgs; [
    adwaita-icon-theme
    jetbrains-mono
    nerd-fonts.jetbrains-mono
    nerd-fonts.ubuntu
    nerd-fonts.ubuntu-mono
  ];
}
