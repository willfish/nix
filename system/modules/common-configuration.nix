{ pkgs, pkgs-unstable, ... }:
{
  system.activationScripts.binBash = ''
    mkdir -p /bin
    ln -sf ${pkgs.bash}/bin/bash /bin/bash
  '';
  system.activationScripts.usrLocal = ''
    mkdir -p /usr/local/bin
    chmod 755 /usr/local/bin
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
  services.elasticsearch.enable = true;
  services.mullvad-vpn.enable = true;
  services.mullvad-vpn.package = pkgs-unstable.mullvad-vpn;

  services.avahi = {
    enable = true;
    nssmdns4 = true;
    openFirewall = true;
  };

  networking.firewall.trustedInterfaces = [ "lo" ];

  # Enable sound with pipewire.
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
      };
    };
  };

  programs.gnupg.agent = {
    enable = true;
    enableSSHSupport = true;
  };

  environment.systemPackages = with pkgs-unstable; [
    neovim
    curl
    git
    ghostty

    openssl # Cryptographic library for SSL/TLS
    openssl.dev # Development files for OpenSSL (headers, libs)
    pkg-config # Helper tool to manage library dependencies during compilation

    home-manager # Nix-based user environment manager

    gnomeExtensions.auto-move-windows # GNOME extension for automatic window positioning
    gnomeExtensions.appindicator # GNOME extension for app indicators
    gnomeExtensions.pop-shell # GNOME extension for tiling window management
    gnome-tweaks # GNOME app for customizing the desktop environment
    pop-launcher # GNOME app for launching applications

    xclip
  ];
  environment.gnome.excludePackages = with pkgs; [
    geary
    gnome-disk-utility
    gnome-backgrounds
    gnome-tour
    gnome-user-docs
    baobab
    epiphany
    gnome-text-editor
    gnome-characters
    gnome-contacts
    gnome-font-viewer
    totem
    yelp
    gnome-software
  ];

  environment.shells = with pkgs-unstable; [
    bash
    fish
  ];
  users.defaultUserShell = pkgs-unstable.fish;
  programs.fish = {
    enable = true;
    package = pkgs-unstable.fish;
  };

  virtualisation.docker.enable = true;

  services = {
    openssh = {
      enable = true;
      settings.PasswordAuthentication = false;
      settings.KbdInteractiveAuthentication = false;
    };

    spice-vdagentd.enable = true;

    displayManager.gdm.enable = true;
    desktopManager.gnome.enable = true;
    xserver = {
      xkb.layout = "us";
      xkb.variant = "";
      enable = true;
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
      substituters = [
        "https://cache.nixos.org"
        # "https://nixpkgs-ruby.cachix.org" - NOTE: Cachix is down
      ];
      warn-dirty = false;
      experimental-features = [
        "nix-command"
        "flakes"
      ];
      auto-optimise-store = true;
    };

    gc = {
      automatic = true;
      dates = "weekly";
      options = "--delete-older-than 7d";
    };
  };
}
