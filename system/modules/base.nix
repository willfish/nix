{ pkgs, ... }:
{
  imports = [ ./tailscale.nix ];

  system.activationScripts.usrLocal = ''
    mkdir -p /usr/local/bin
    chmod 755 /usr/local/bin
  '';

  boot.loader.systemd-boot.enable = true;
  boot.loader.efi.canTouchEfiVariables = true;
  networking.networkmanager.enable = true;
  networking.firewall.trustedInterfaces = [ "lo" ];

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

  users.users.william = {
    isNormalUser = true;
    description = "William Fish";
    extraGroups = [ "wheel" ];
  };

  programs.gnupg.agent = {
    enable = true;
    enableSSHSupport = true;
  };

  environment.systemPackages = with pkgs; [
    neovim
    curl
    git
    home-manager
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

  services.openssh = {
    enable = true;
    authorizedKeysFiles = [
      ".ssh/authorized_keys"
      "/run/secrets/ssh/authorized_keys.d/%u"
    ];
    settings.PasswordAuthentication = false;
    settings.KbdInteractiveAuthentication = false;
  };

  documentation.nixos.enable = false;

  nix = {
    settings = {
      substituters = [
        "https://cache.nixos.org"
        "https://cache.numtide.com"
      ];
      trusted-public-keys = [
        "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY="
        "niks3.numtide.com-1:DTx8wZduET09hRmMtKdQDxNNthLQETkc/yaX7M4qK0g="
      ];
      accept-flake-config = false;
      require-sigs = true;
      sandbox = true;
      trusted-users = [
        "root"
        "william"
      ];
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
