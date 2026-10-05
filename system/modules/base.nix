{ config, pkgs, ... }:

let
  # Pushes every locally built output's closure to the terminus binary cache.
  # Runs as root from nix-daemon after each build; must never fail a build.
  # The push key's forced command on terminus only accepts cache imports.
  postBuildHook = pkgs.writeShellScript "nix-post-build-cache-push" ''
    set -u

    [ -n "''${OUT_PATHS:-}" ] || exit 0
    [ "$(cat /proc/sys/kernel/hostname)" != "terminus" ] || exit 0

    key=/run/secrets/TERMINUS_CACHE_PUSH_KEY
    [ -r "$key" ] || exit 0

    target=""
    for candidate in terminus.local terminus; do
      if ${pkgs.netcat-openbsd}/bin/nc -z -w 2 "$candidate" 22 </dev/null >/dev/null 2>&1; then
        target="$candidate"
        break
      fi
    done
    [ -n "$target" ] || exit 0

    ssh_cmd=(
      ${pkgs.openssh}/bin/ssh
      -i "$key"
      -o IdentitiesOnly=yes
      -o BatchMode=yes
      -o ConnectTimeout=3
      -o StrictHostKeyChecking=yes
      -o UserKnownHostsFile=/etc/ssh/ssh_known_hosts
    )

    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT

    read -ra outputs <<< "$OUT_PATHS"
    ${config.nix.package}/bin/nix-store -qR "''${outputs[@]}" > "$work/closure" || exit 0
    "''${ssh_cmd[@]}" "william@$target" missing < "$work/closure" > "$work/delta" || exit 0
    if [ -s "$work/delta" ]; then
      # shellcheck disable=SC2046 # store paths contain no whitespace
      ${config.nix.package}/bin/nix-store --export $(cat "$work/delta") | "''${ssh_cmd[@]}" "william@$target" import || exit 0
    fi
    "''${ssh_cmd[@]}" "william@$target" root "''${outputs[@]}" || true
    exit 0
  '';
in
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

  # Pinned so the post-build-hook can push without interactive host-key prompts.
  programs.ssh.knownHosts = {
    terminus.publicKey = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMkRiuJILkDT0xvE1YcVehmGrvtDTyThKQSf9Zm56Eix";
    "terminus.local".publicKey =
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMkRiuJILkDT0xvE1YcVehmGrvtDTyThKQSf9Zm56Eix";
  };

  services.openssh = {
    enable = true;
    settings.PasswordAuthentication = false;
    settings.KbdInteractiveAuthentication = false;
  };

  # graphical-desktop turns speechd on with X. Nothing on these hosts uses it.
  services.speechd.enable = false;

  documentation.nixos.enable = false;

  nix = {
    settings = {
      substituters = [
        # Terminus harmonia, tailnet-only; priority 30 beats cache.nixos.org.
        "http://terminus:5000"
        "https://cache.nixos.org"
        "https://cache.numtide.com"
        "https://herdr.cachix.org"
      ];
      trusted-public-keys = [
        "terminus-cache-1:qi0G59n0fBbltiVE9Udsnonnjcze2bm+iA3a5frrQUQ="
        "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY="
        "niks3.numtide.com-1:DTx8wZduET09hRmMtKdQDxNNthLQETkc/yaX7M4qK0g="
        "herdr.cachix.org-1:3nH7IStRsS0ASfdonA0DCRR2ZrSCeWitZ7Kwew0cR4I="
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
      post-build-hook = "${postBuildHook}";
    };

    gc = {
      automatic = true;
      dates = "weekly";
      options = "--delete-older-than 7d";
    };
  };
}
