{
  config,
  lib,
  pkgs,
  homeConfiguration,
  ...
}:
let
  home = homeConfiguration.home.homeDirectory;
  user = homeConfiguration.home.username;
  secrets = homeConfiguration.privateConfig.darwinSecretsPackage;
  # Never inherit a graphical login's environment or SSH agent.
  environment = {
    HOME = home;
    USER = user;
    LOGNAME = user;
    SOPS_AGE_KEY_FILE = homeConfiguration.home.sessionVariables.SOPS_AGE_KEY_FILE;
    PATH = lib.concatStringsSep ":" [
      "${home}/.hermes/hermes-agent/venv/bin"
      "${home}/.local/bin"
      "${home}/.bin"
      "${home}/.nix-profile/bin"
      (lib.makeBinPath [
        pkgs.uv
        pkgs.nodejs_24
        pkgs.git
        pkgs.openssh
        pkgs.coreutils
      ])
      "/usr/bin"
      "/bin"
      "/usr/sbin"
      "/sbin"
    ];
  };
  makeJob =
    name: spec:
    let
      clean = lib.filterAttrs (_: value: value != null) spec;
      args = lib.optional ((spec.Program or null) != null) spec.Program ++ (spec.ProgramArguments or [ ]);
      start = pkgs.writeShellScript "start-${name}" ''
        set -euo pipefail
        ${secrets}/bin/darwin-secrets wait
        exec ${lib.escapeShellArgs args}
      '';
    in
    {
      serviceConfig =
        (removeAttrs clean [
          "Program"
          "ProgramArguments"
          "Label"
        ])
        // {
          Label = "org.nixos.${name}";
          UserName = user;
          GroupName = "staff";
          Umask = 63; # Decimal 0077, as required by launchd's integer field.
          EnvironmentVariables = environment // (clean.EnvironmentVariables or { });
          ProgramArguments = [
            "/bin/sh"
            "-c"
            "/bin/wait4path /nix/store && exec ${start}"
          ];
        };
    };
in
{
  # Determinate owns the installed Nix daemon and nix.conf on Relay.
  nix.enable = false;
  system.primaryUser = user;
  system.stateVersion = 6;
  networking.computerName = config.networking.hostName;
  networking.localHostName = config.networking.hostName;

  power = {
    sleep.computer = "never";
    sleep.display = 10;
    restartAfterPowerFailure = true;
  };
  system.defaults.loginwindow = {
    GuestEnabled = false;
    autoLoginUser = null;
  };
  security.pam.services.sudo_local.touchIdAuth = false;
  services.openssh.enable = true;

  launchd.daemons = lib.mapAttrs makeJob homeConfiguration.dotfiles.darwinDaemons // {
    dotfiles-secrets.serviceConfig = {
      Label = "org.nixos.dotfiles-secrets";
      UserName = user;
      GroupName = "staff";
      Umask = 63;
      EnvironmentVariables = environment;
      ProgramArguments = [
        "/bin/sh"
        "-c"
        "/bin/wait4path /nix/store && exec ${secrets}/bin/darwin-secrets provision"
      ];
      RunAtLoad = true;
      KeepAlive.SuccessfulExit = false;
      ThrottleInterval = 30;
      StandardOutPath = "${home}/Library/Logs/dotfiles-secrets.log";
      StandardErrorPath = "${home}/Library/Logs/dotfiles-secrets.log";
    };
    tailscale.serviceConfig = {
      Label = "org.nixos.tailscale";
      ProgramArguments = [
        "/bin/sh"
        "-c"
        "/bin/wait4path /nix/store && exec ${pkgs.tailscale}/bin/tailscaled --state=/var/lib/tailscale/tailscaled.state --socket=/var/run/tailscaled.socket"
      ];
      RunAtLoad = true;
      KeepAlive = true;
      ThrottleInterval = 30;
      StandardOutPath = "/var/log/tailscaled.log";
      StandardErrorPath = "/var/log/tailscaled.log";
    };
    maxfiles.serviceConfig = {
      Label = "org.nixos.maxfiles";
      ProgramArguments = [
        "/bin/launchctl"
        "limit"
        "maxfiles"
        "65536"
        "245760"
      ];
      RunAtLoad = true;
    };
  };

  # A switch is not permission to stop the existing automation or its network.
  # The approved handover must retire these definitions before first activation.
  system.activationScripts.preActivation.text = ''
    if /usr/bin/defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser >/dev/null 2>&1; then
      echo "Disable automatic graphical login through the approved setup before activation." >&2
      exit 1
    fi
    uid=$(/usr/bin/id -u ${lib.escapeShellArg user})
    for label in \
      ai.hermes.gateway \
      org.nix-community.home.sops-nix \
      org.nix-community.home.local-llm \
      org.nix-community.home.local-assistant-tools
    do
      if [ -e ${lib.escapeShellArg "${home}/Library/LaunchAgents"}/"$label.plist" ] \
        || /bin/launchctl print "gui/$uid/$label" >/dev/null 2>&1; then
        echo "Refusing handover while graphical job $label is installed or loaded; follow the approved migration runbook." >&2
        exit 1
      fi
    done
    for plist in io.tailscale.tailscaled limit.maxfiles; do
      if [ -e "/Library/LaunchDaemons/$plist.plist" ] \
        || /bin/launchctl print "system/$plist" >/dev/null 2>&1; then
        echo "Retire the unmanaged $plist definition during the approved handover first." >&2
        exit 1
      fi
    done
    /usr/bin/install -d -o ${lib.escapeShellArg user} -g staff -m 0700 ${lib.escapeShellArg "${home}/Library/Logs"}
    /usr/bin/install -d -o root -g wheel -m 0700 /var/lib/tailscale
  '';

  assertions = [
    {
      assertion = homeConfiguration.dotfiles.darwinSystemServices;
      message = "The headless system and Home Manager service ownership must agree.";
    }
  ];
}
