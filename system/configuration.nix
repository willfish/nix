# Edit this configuration file to define what should be installed on
# your system.  Help is available in the configuration.nix(5) man page
# and in the NixOS manual (accessible by running ‘nixos-help’).

{ pkgs, pkgs-unstable, ... }:

{
  system.stateVersion = "23.11";
  imports = [./hardware-configuration.nix];


  # Bootloader.
  boot.loader.systemd-boot.enable = true;
  boot.loader.efi.canTouchEfiVariables = true;

  networking.hostName = "andromeda"; # Define your hostname.
  # networking.wireless.enable = true;  # Enables wireless support via wpa_supplicant.

  # Configure network proxy if necessary
  # networking.proxy.default = "http://user:password@proxy:port/";
  # networking.proxy.noProxy = "127.0.0.1,localhost,internal.domain";

  # Enable networking
  networking.networkmanager.enable = true;

  # Set your time zone.
  time.timeZone = "Europe/London";

  # Select internationalisation properties.
  i18n.defaultLocale = "en_GB.UTF-8";

  i18n.extraLocaleSettings = {
    LC_ADDRESS = "en_GB.UTF-8";
    LC_IDENTIFICATION = "en_GB.UTF-8";
    LC_MEASUREMENT = "en_GB.UTF-8";
    LC_MONETARY = "en_GB.UTF-8";
    LC_NAME = "en_GB.UTF-8";
    LC_NUMERIC = "en_GB.UTF-8";
    LC_PAPER = "en_GB.UTF-8";
    LC_TELEPHONE = "en_GB.UTF-8";
    LC_TIME = "en_GB.UTF-8";
  };


  # Enable CUPS to print documents.
  services.printing.enable = true;

  services.avahi = {
    enable = true;
    nssmdns = true;
    openFirewall = true;
  };

  hardware.system76.enableAll = true;
  # Enable sound with pipewire.
  sound.enable = true;
  hardware.pulseaudio.enable = false;

  security.rtkit.enable = true;
  security.pam.services.swaylock = {};
  security.pam.services.swaylock.fprintAuth = false;

  services.pipewire = {
    enable = true;
    alsa.enable = true;
    alsa.support32Bit = true;
    pulse.enable = true;
  };

  # Enable touchpad support (enabled default in most desktopManager).
  # services.xserver.libinput.enable = true;

  # Define a user account. Don't forget to set a password with ‘passwd’.
  users.users.william = {
    isNormalUser = true;
    description = "William Fish";
    extraGroups = [ "networkmanager" "wheel" "docker" "audio" "video" "libvirt" "input" "bluetooth" ];
    packages = with pkgs; [
      docker
      docker-compose
    ];

    openssh.authorizedKeys.keys = [
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEodOKhGXEeHpinEPh5po/D+RTmLXPoMbdjtR2ESxGVi william@andromeda"
    ];
  };

  services.blueman.enable = true;

  hardware.bluetooth = {
    enable = true;
    powerOnBoot = true;
  };

  programs.gnupg.agent = {
    enable = true;
    enableSSHSupport = true;
  };

  virtualisation.docker.enable = true;

  environment.systemPackages = with pkgs-unstable; [
    # Minimum packages
    vim
    neovim
    curl
    git
    kitty

    adwaita-icon-theme # default gnome cursors
    glib
    gsettings-desktop-schemas
    nwg-look

    lm_sensors
    libsForQt5.qt5.qtquickcontrols2
    libsForQt5.qt5.qtgraphicaleffects
    libsForQt5.qt5.qtsvg
    openssl
    openssl.dev
    pkg-config
    xfce.thunar
  ];
  environment.shells = with pkgs; [bash fish];
  users.defaultUserShell = pkgs.fish;
  programs.fish.enable = true;

  services = {
    openssh = {
      enable = true;
      settings.PasswordAuthentication = false;
      settings.KbdInteractiveAuthentication = false;
    };

    dbus.enable = true;
    spice-vdagentd.enable = true;

    # services.xserver.displayManager.gdm.enable = true;
    xserver = {
      enable = true;
      layout = "us";
      xkbVariant = "";
      # xkbOptions = "grp:alt_shift_toggle, caps:swapescape";
      windowManager.i3 = {
        enable = true;
        extraPackages = with pkgs-unstable; [
          i3status
          i3lock
          i3lock-blur
          i3blocks
          maim
          rofi
          dmenu
          picom
          dunst
          pa_applet
          networkmanagerapplet
          polybar
          libayatana-indicator-gtk3
          nitrogen
          feh
          xautolock
          gexiv2
          libnotify
          gtk3
          imagemagick
          dconf
          # xdg-desktop-portal
        ];
      };

      displayManager = {
        sddm.enable = true;
        sddm.theme = "${import ./modules/sddm-theme.nix { inherit pkgs; }}";
      };
    };
  };

  #   building the system configuration...
  # trace: warning: xdg-desktop-portal 1.17 reworked how portal implementations are loaded, you
  # should either set `xdg.portal.config` or `xdg.portal.configPackages`
  # to specify which portal backend to use for the requested interface.
  #
  # https://github.com/flatpak/xdg-desktop-portal/blob/1.18.1/doc/portals.conf.rst.in
  #
  # If you simply want to keep the behaviour in < 1.17, which uses the first
  # portal implementation found in lexicographical order, use the following:
  #
  # xdg.portal.config.common.default = "*";
  # xdg.portal = {
  #   enable = true;
  #   wlr.enable = false;
  #   xdgOpenUsePortal = true;
  #   extraPortals = [
  #     pkgs-unstable.xdg-desktop-portal-gtk
  #   ];
  # };


  fonts = {
    packages = with pkgs; [
      fira-code
      font-awesome
      jetbrains-mono
      nerdfonts
    ];
  };

  systemd = {
    user.services.polkit-gnome-authentication-agent-1 = {
        description = "polkit-gnome-authentication-agent-1";
        wantedBy = [ "graphical-session.target" ];
        wants = [ "graphical-session.target" ];
        after = [ "graphical-session.target" ];
        serviceConfig = {
            Type = "simple";
            ExecStart = "${pkgs.polkit_gnome}/libexec/polkit-gnome-authentication-agent-1";
            Restart = "on-failure";
            RestartSec = 1;
            TimeoutStopSec = 10;
        };
    };
    extraConfig = ''
        DefaultTimeoutStopSec=10s
    '';
  };

  documentation.nixos.enable = false;

  nix = {
    settings = {
      warn-dirty = false;
      experimental-features = [ "nix-command" "flakes" ];
      auto-optimise-store = true;
    };

    gc = {
      automatic = true;
      dates = "weekly";
      options = "--delete-older-than 7d";
    };
  };

 programs = {
   steam = {
     enable = true;
     remotePlay.openFirewall = true;
     dedicatedServer.openFirewall = true;
   };
 };
}
