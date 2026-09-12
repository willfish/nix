{
  config,
  lib,
  pkgs,
  homeConfiguration,
  ...
}:
let
  appNames = builtins.attrNames homeConfiguration.dotfiles.darwinDaemons;
  logs = map (name: "/var/log/dotfiles/${name}.log") (
    appNames
    ++ [
      "secrets"
      "tailscale"
      "health"
    ]
  );
  healthConfig = pkgs.writeText "darwin-health.json" (
    builtins.toJSON {
      services =
        map (name: {
          label = "org.nixos.${name}";
          kind = "running";
        }) (appNames ++ [ "tailscale" ])
        ++ [
          {
            label = "org.nixos.sops-install-secrets";
            kind = "oneshot";
          }
          {
            label = "com.openssh.sshd";
            kind = "socket";
          }
        ];
      readiness = [
        "${homeConfiguration.privateConfig.darwinSecretsPackage}/bin/darwin-secrets"
        "wait"
        "--timeout"
        "0"
        "--home-generation"
        "${homeConfiguration.home.activationPackage}"
      ];
      inherit logs;
      statusFile = "/var/db/dotfiles/health.json";
      logLimitBytes = 5 * 1024 * 1024;
    }
  );
  health = pkgs.writeShellScriptBin "darwin-health" ''
    exec ${pkgs.python3}/bin/python3 ${./scripts/health.py} --config ${healthConfig} "$@"
  '';
in
{
  environment.systemPackages = [ health ];
  launchd.daemons.dotfiles-health.serviceConfig = {
    Label = "org.nixos.dotfiles-health";
    ProgramArguments = [
      "/bin/sh"
      "-c"
      "/bin/wait4path /nix/store && exec ${health}/bin/darwin-health --record"
    ];
    RunAtLoad = true;
    StartInterval = 300;
    KeepAlive = false;
    ProcessType = "Background";
    LowPriorityIO = true;
    Nice = 15;
    Umask = 63;
    StandardOutPath = "/var/log/dotfiles/health.log";
    StandardErrorPath = "/var/log/dotfiles/health.log";
  };
  # Owned directories prevent unprivileged replacement of log paths with links.
  system.activationScripts.preActivation.text = lib.mkOrder 1100 ''
    /usr/bin/install -d -o root -g wheel -m 0755 /var/log/dotfiles /var/db/dotfiles
    ${lib.concatMapStringsSep "\n"
      (
        name:
        let
          owner = if builtins.elem name appNames then homeConfiguration.home.username else "root";
          group = if owner == "root" then "wheel" else "staff";
          path = "/var/log/dotfiles/${name}.log";
        in
        ''
          if [ -L ${path} ] || { [ -e ${path} ] && [ ! -f ${path} ]; }; then
            echo "Refusing unsafe log path ${path}" >&2
            exit 1
          fi
          if [ ! -e ${path} ]; then
            /usr/bin/install -o ${lib.escapeShellArg owner} -g ${group} -m 0600 /dev/null ${path}
          fi
        ''
      )
      (
        appNames
        ++ [
          "secrets"
          "tailscale"
          "health"
        ]
      )
    }
  '';
  assertions = [
    {
      assertion = config.launchd.daemons.dotfiles-health.serviceConfig.StartInterval >= 60;
      message = "Health collection must not become a tight polling loop.";
    }
  ];
}
