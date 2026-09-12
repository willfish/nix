{
  config,
  lib,
  pkgs,
  homeConfiguration,
  ...
}:
let
  inherit (homeConfiguration) home;
  specs = builtins.attrValues homeConfiguration.dotfiles.darwinDaemons;
  runtimeFiles = lib.concatMap (
    s:
    let
      args = s.ProgramArguments or [ ];
    in
    lib.optional (args != [ ] && lib.hasPrefix home.homeDirectory (builtins.head args)) (
      builtins.head args
    )
  ) specs;
  modelFiles = lib.concatMap (
    s:
    if builtins.isAttrs (s.KeepAlive or null) then
      builtins.attrNames (s.KeepAlive.PathState or { })
    else
      [ ]
  ) specs;
  contract = pkgs.writeText "darwin-server-policy.json" (
    builtins.toJSON {
      schema = 1;
      host = config.networking.hostName;
      architecture = if pkgs.stdenv.hostPlatform.isAarch64 then "arm64" else "x86_64";
      user = home.username;
      home = home.homeDirectory;
      installedConfig = "/etc/dotfiles/server.json";
      systemSecrets = "/run/secrets";
      legacySystemDirectory = "/Library/LaunchDaemons";
      homeGenerationProfile = "${homeConfiguration.xdg.stateHome}/nix/profiles/home-manager";
      homeGeneration = "${homeConfiguration.home.activationPackage}";
      jobs =
        lib.genAttrs
          (
            (builtins.attrNames homeConfiguration.dotfiles.darwinDaemons)
            ++ [
              "sops-install-secrets"
              "tailscale"
              "maxfiles"
              "dotfiles-health"
            ]
          )
          (name: {
            # Detect definition changes without displaying service environments.
            definitionHash = builtins.hashString "sha256" (
              builtins.toJSON config.launchd.daemons.${name}.serviceConfig
            );
          });
      operatorTools = lib.genAttrs [ "deploy" "preflight" "health" ] (
        name: builtins.hashFile "sha256" (./scripts + "/${name}.py")
      );
      legacyUserJobs = [
        "ai.hermes.gateway"
        "org.nix-community.home.sops-nix"
        "org.nix-community.home.local-llm"
        "org.nix-community.home.local-assistant-tools"
      ];
      legacySystemJobs = [
        "io.tailscale.tailscaled"
        "limit.maxfiles"
        "org.nixos.dotfiles-secrets"
      ];
      requiredFiles = runtimeFiles ++ modelFiles;
      executables = runtimeFiles;
      serviceNames = builtins.attrNames homeConfiguration.dotfiles.darwinDaemons;
      secretsManifest = "${config.system.build.sops-nix-manifest}";
      secretGroups = homeConfiguration.privateConfig.secretGroups;
      updates =
        config.system.defaults.CustomSystemPreferences."/Library/Preferences/com.apple.SoftwareUpdate";
      nixManagedByDarwin = config.nix.enable;
    }
  );
  preflight = pkgs.writeShellScriptBin "darwin-preflight" ''
    exec ${pkgs.python3}/bin/python3 ${./scripts/preflight.py} --config ${contract} "$@"
  '';
  deploy = pkgs.writeShellScriptBin "darwin-deploy" ''
    exec ${pkgs.python3}/bin/python3 ${./scripts/deploy.py} "$@"
  '';
in
{
  environment.systemPackages = [
    preflight
    deploy
  ];
  environment.etc."dotfiles/server.json".source = contract;
  environment.etc."dotfiles/home-generation".text = "${homeConfiguration.home.activationPackage}\n";
  # Runs before nix-darwin's mutation phases, and independently before profile
  # registration when using darwin-deploy. It never stops a legacy job.
  system.activationScripts.preActivation.text = lib.mkOrder 500 ''
    ${preflight}/bin/darwin-preflight --target "$systemConfig"
    /usr/bin/install -d -o ${lib.escapeShellArg home.username} -g staff -m 0700 ${lib.escapeShellArg "${home.homeDirectory}/Library/Logs"}
    /usr/bin/install -d -o root -g wheel -m 0700 /var/lib/tailscale
  '';
}
