{
  lib,
  pkgs,
  homeConfiguration,
  ...
}:
let
  home = homeConfiguration.home.homeDirectory;
  user = homeConfiguration.home.username;
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
      args = lib.optional (clean ? Program) clean.Program ++ (clean.ProgramArguments or [ ]);
      start = pkgs.writeShellScript "start-${name}" ''
        set -euo pipefail
        ${homeConfiguration.privateConfig.darwinSecretsPackage}/bin/darwin-secrets wait \
          --home-generation ${homeConfiguration.home.activationPackage}
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
          Umask = 63;
          EnvironmentVariables = environment // (clean.EnvironmentVariables or { });
          ProgramArguments = [
            "/bin/sh"
            "-c"
            "/bin/wait4path /nix/store && exec ${start}"
          ];
          StandardOutPath = "/var/log/dotfiles/${name}.log";
          StandardErrorPath = "/var/log/dotfiles/${name}.log";
        };
    };
in
{
  launchd.daemons = lib.mapAttrs makeJob homeConfiguration.dotfiles.darwinDaemons // {
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
      StandardOutPath = "/var/log/dotfiles/tailscale.log";
      StandardErrorPath = "/var/log/dotfiles/tailscale.log";
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
}
