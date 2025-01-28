{ pkgs, pkgs-unstable, ... }:
{
  boot.loader.systemd-boot.enable = true;
  boot.loader.efi.canTouchEfiVariables = true;
  boot.kernelParams = [ "btiso.enable=1" ];

  networking.extraHosts = ''
    127.0.0.1 host.docker.internal
  '';
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
    extraGroups = [ "networkmanager" "wheel" "docker" "audio" "video" "libvirt" "input" "bluetooth" ];
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
    # Minimum packages
    vim
    neovim
    curl
    git
    kitty
    ghostty

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

    bluez
    home-manager

    ghc
  ];
  environment.shells = with pkgs; [ bash fish ];
  users.defaultUserShell = pkgs.fish;
  programs.fish.enable = true;

  virtualisation.docker.enable = true;

  services = {
    displayManager = {
      sddm.enable = true;
      sddm.theme = "${import ../modules/sddm-theme.nix { inherit pkgs-unstable; }}";
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
          xmonad
          xmonad-contrib
          xmonad-extras
          xmonad-utils
        ];
      };
      windowManager.i3 = {
        enable = true;
        extraPackages = with pkgs-unstable; [
          dconf
          dmenu
          dunst
          feh
          gexiv2
          gtk3
          i3blocks
          i3lock-blur
          i3status
          imagemagick
          libayatana-indicator-gtk3
          libnotify
          maim
          networkmanagerapplet
          nitrogen
          pa_applet
          picom
          polybar
          rofi
          swappy
          xautolock
        ];
      };
    };

    ollama = {
      enable = true;
      package = pkgs-unstable.ollama;
    };
  };

  fonts = {
    packages = with pkgs; [
      fira-code
      font-awesome
      jetbrains-mono
      nerdfonts
      powerline-fonts
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

  environment.etc."polkit-1/rules.d/50-nopasswd.rules" = {
    text = ''
      polkit.addRule(function(action, subject) {
          if (subject.isInGroup("wheel")) {
              return polkit.Result.YES;
          }
      });
    '';
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
