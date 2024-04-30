# Edit this configuration file to define what should be installed on
# your system.  Help is available in the configuration.nix(5) man page
# and in the NixOS manual (accessible by running ‘nixos-help’).

{ config, pkgs, pkgs-unstable, ... }:

{
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


  programs = {
    hyprland = {
      enable = true;

      xwayland = {
        enable = true;
      };

      portalPackage = pkgs.xdg-desktop-portal-hyprland;
    };
  };

  # Enable CUPS to print documents.
  services.printing.enable = true;

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
    # If you want to use JACK applications, uncomment this
    #jack.enable = true;

    # use the example session manager (no others are packaged yet so this is enabled by default,
    # no need to redefine it in your config for now)
    #media-session.enable = true;
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

  # Allow unfree packages
  # nixpkgs.config.allowUnfree = true;
  # unstable.config.allowUnfree = true;

  environment.systemPackages = with pkgs; [
    # Minimum packages
    pkgs-unstable.vim
    pkgs-unstable.neovim
    pkgs-unstable.curl
    pkgs-unstable.git

    # Hyprland
    pkgs-unstable.hyprpaper
    pkgs-unstable.kitty
    pkgs-unstable.libnotify
    pkgs-unstable.mako
    pkgs-unstable.qt5.qtwayland
    pkgs-unstable.qt6.qtwayland
    pkgs-unstable.swayidle
    pkgs-unstable.swaylock-effects
    pkgs-unstable.wlogout
    pkgs-unstable.wl-clipboard
    pkgs-unstable.wofi
    pkgs-unstable.waybar

    pkgs-unstable.gnome3.adwaita-icon-theme # default gnome cursors
    pkgs-unstable.glib
    pkgs-unstable.gsettings-desktop-schemas
    pkgs-unstable.nwg-look

    pkgs.lm_sensors
    pkgs-unstable.libsForQt5.qt5.qtquickcontrols2
    pkgs-unstable.libsForQt5.qt5.qtgraphicaleffects
    pkgs-unstable.libsForQt5.qt5.qtsvg
    pkgs-unstable.openssl
    pkgs-unstable.openssl.dev
    pkgs-unstable.pkg-config
    pkgs-unstable.xfce.thunar
    pkgs-unstable.xdg-desktop-portal-gtk
    pkgs-unstable.xdg-desktop-portal-wlr
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

      displayManager = {
        sddm.enable = true;
        sddm.theme = "${import ./modules/sddm-theme.nix { inherit pkgs; }}";
      };
    };
  };

  system.stateVersion = "23.11";


  fonts = {
    packages = with pkgs; [
      jetbrains-mono
      nerdfonts
      font-awesome
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

 xdg.portal = {
    enable = true;
    wlr.enable = false;
    xdgOpenUsePortal = false;
    extraPortals = [
      pkgs.xdg-desktop-portal-hyprland
      pkgs.xdg-desktop-portal-gtk
    ];
 };
}
