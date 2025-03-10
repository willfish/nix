{ pkgs, pkgs-unstable, ... }:
let
  sddm-astronaut = pkgs-unstable.sddm-astronaut.override {
    themeConfig = {
      AccentColor = "#746385";
      FormPosition = "left";

      ForceHideCompletePassword = true;
    };
    embeddedTheme = "hyprland_kath";
  };
in
{
  system.activationScripts.binBash = ''
    mkdir -p /bin
    ln -sf ${pkgs.bash}/bin/bash /bin/bash
  '';

  boot.loader.systemd-boot.enable = true;
  boot.loader.efi.canTouchEfiVariables = true;
  boot.kernelParams = [ "btiso.enable=1" ];

  networking.extraHosts = ''
    127.0.0.1 host.docker.internal
  '';

  networking.networkmanager.enable = true;

  time.timeZone = "Europe/London";

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
    nssmdns4 = true;
    openFirewall = true;
  };

  # Enable sound with pipewire.
  hardware.pulseaudio.enable = false;

  security.rtkit.enable = true;

  services.pipewire = {
    enable = true;
    alsa.enable = true;
    alsa.support32Bit = true;
    pulse.enable = true;
    wireplumber.enable = true;
    extraConfig = {
      pipewire = {
        "switch-on-connect" = {
          "pulse.cmd" = [
            {
              cmd = "load-module";
              args = "module-always-sink";
              flags = [ ];
            }
            {
              cmd = "load-module";
              args = "module-switch-on-connect";
            }
          ];
        };
      };
    };
  };

  users.users.william = {
    isNormalUser = true;
    description = "William Fish";
    extraGroups = [
      "audio"
      "bluetooth"
      "docker"
      "input"
      "libvirt"
      "networkmanager"
      "video"
      "wheel"
    ];
    packages = with pkgs; [
      docker
      docker-compose
    ];

    openssh.authorizedKeys.keys = [
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEodOKhGXEeHpinEPh5po/D+RTmLXPoMbdjtR2ESxGVi william@andromeda"
    ];
  };

  hardware.bluetooth = {
    enable = true;
    powerOnBoot = true;
    settings = {
      General = {
        Experimental = true;
        DebugKeys = true;
        Enable = "Source,Sink,Media,Socket";
      };
    };
  };

  services.blueman.enable = true;

  programs.gnupg.agent = {
    enable = true;
    enableSSHSupport = true;
  };

  environment.systemPackages = with pkgs-unstable; [
    neovim                 # Modernized fork of Vim with additional features
    curl                   # Command-line tool for transferring data with URLs
    git                    # Distributed version control system
    kitty                  # Fast, feature-rich, GPU-based terminal emulator
    ghostty                # Minimalist, high-performance terminal emulator

    adwaita-icon-theme     # Default GNOME icon theme for consistent UI icons
    glib                   # Core library for GNOME applications (e.g., GSettings)
    gsettings-desktop-schemas  # GSettings schemas for desktop settings
    nwg-look               # GUI tool to customize GTK themes and settings

    lm_sensors             # Tools to monitor hardware sensors (e.g., temperature)
    openssl                # Cryptographic library for SSL/TLS
    openssl.dev            # Development files for OpenSSL (headers, libs)
    pkg-config             # Helper tool to manage library dependencies during compilation

    bluez                  # Bluetooth protocol stack for Linux
    home-manager           # Nix-based user environment manager

    xmonad-with-packages   # Xmonad window manager with bundled Haskell dependencies
    ghc                    # Glasgow Haskell Compiler for building Haskell apps

    sddm-astronaut         # Custom SDDM theme (login screen) defined above

    libsForQt5.qt5.qtquickcontrols2  # Qt5 module for QML-based UI controls
    libsForQt5.qt5.qtgraphicaleffects # Qt5 module for graphical effects in QML
    libsForQt5.qt5.qtsvg             # Qt5 module for SVG rendering

    feh                    # Lightweight image viewer and wallpaper setter
    gtk3                   # GTK+ 3 library for GUI applications
    maim                   # Screenshot utility for X11
    networkmanagerapplet   # System tray applet for NetworkManager
    pa_applet              # PulseAudio system tray applet
    picom                  # Compositor for X11 (window effects like transparency)
    polybar                # Lightweight, customizable status bar for X11
    rofi                   # Application launcher and window switcher
    swappy                 # Screenshot editing tool (e.g., annotate, crop)
    xautolock              # Automatically locks the screen after inactivity
    xclip                  # Command-line clipboard manager
    libnotify              # Library for desktop notifications
    libayatana-indicator-gtk3 # GTK3 library for Ayatana indicators
  ];

  environment.shells = with pkgs-unstable; [ bash fish ];
  users.defaultUserShell = pkgs-unstable.fish;
  programs.fish = {
    enable = true;
    package = pkgs-unstable.fish;
  };

  virtualisation.docker.enable = true;

  services = {
    displayManager = {
      sddm = {
        enable = true;
        package = pkgs-unstable.kdePackages.sddm;

        theme = "sddm-astronaut-theme";
        extraPackages = [ sddm-astronaut ];
      };
    };

    openssh = {
      enable = true;
      settings.PasswordAuthentication = false;
      settings.KbdInteractiveAuthentication = false;
    };

    dbus = {
      enable = true;
      packages = with pkgs-unstable; [
        bluez
      ];
    };
    spice-vdagentd.enable = true;

    xserver = {
      xkb.layout = "us";
      xkb.variant = "";
      enable = true;
      windowManager.xmonad = {
        enable = true;
        config = builtins.readFile ../../home/config/xmonad/xmonad.hs;
        extraPackages = haskellPackages: with haskellPackages; [
          xmonad-contrib
          xmonad-extras
          xmonad-utils
        ];
        enableConfiguredRecompile = true;
      };
    };
  };

  fonts = {
    packages = with pkgs-unstable; [
      adwaita-icon-theme
      jetbrains-mono
      nerd-fonts.jetbrains-mono
      nerd-fonts.ubuntu
      nerd-fonts.ubuntu-mono
      powerline-fonts # Used by Dracula Theme in Tmux
    ];
  };

  documentation.nixos.enable = false;

  nix = {
    settings = {
      substituters = [ "https://cache.nixos.org" "https://nixpkgs-ruby.cachix.org" ];
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
}
