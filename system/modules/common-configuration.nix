{ pkgs, ... }:
{
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
  services.opensearch.enable = true;
  services.mullvad-vpn.enable = true;
  services.mullvad-vpn.package = pkgs.mullvad-vpn;

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

  environment.systemPackages = with pkgs; [
    neovim
    curl
    git
    ghostty

    home-manager # Nix-based user environment manager

    xclip
  ];
  environment.shells = with pkgs; [
    bash
    fish
  ];
  users.defaultUserShell = pkgs.fish;
  programs.fish = {
    enable = true;
    package = pkgs.fish;
  };

  virtualisation.docker.enable = true;

  services = {
    openssh = {
      enable = true;
      settings.PasswordAuthentication = false;
      settings.KbdInteractiveAuthentication = false;
    };

    spice-vdagentd.enable = true;

    displayManager.cosmic-greeter.enable = true;
    desktopManager.cosmic.enable = true;
    xserver = {
      xkb.layout = "us";
      xkb.variant = "";
      enable = true;
    };
  };

  fonts = {
    packages = with pkgs; [
      adwaita-icon-theme
      jetbrains-mono
      nerd-fonts.jetbrains-mono
      nerd-fonts.ubuntu
      nerd-fonts.ubuntu-mono
    ];
  };

  documentation.nixos.enable = false;

  nix = {
    settings = {
      substituters = [
        "https://cache.nixos.org"
        # "https://nixpkgs-ruby.cachix.org"
      ];
      warn-dirty = false;
      trusted-users = [ "root" "william" ];
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
