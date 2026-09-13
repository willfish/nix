{
  lib,
  pkgs,
  darwin,
  home,
}:
let
  c = darwin.config;
  daemons = c.launchd.daemons;
  unprivileged = [
    "hermes"
    "local-llm"
    "local-assistant-tools"
  ];
  check = condition: message: lib.assertMsg condition "Headless Darwin: ${message}";
in
assert check (!c.nix.enable) "must not take ownership of Determinate Nix";
assert check c.services.openssh.enable "native SSH must stay enabled";
assert check (lib.hasInfix "diffutils" home.home.activation.configureLocalPi.data)
  "Pi settings comparison must use the package that provides cmp";
assert check (c.networking.hostName == "relay") "wrong node identity";
assert check (
  c.power.sleep.computer == "never" && c.power.restartAfterPowerFailure
) "server power policy";
assert check (c.system.defaults.loginwindow.autoLoginUser == null) "automatic GUI login enabled";
assert check (builtins.all (
  name:
  let
    s = daemons.${name}.serviceConfig;
  in
  s.UserName == "william"
  && s.GroupName == "staff"
  && s.Umask == 63
  && s.EnvironmentVariables.HOME == "/Users/william"
  && s.ProgramArguments != [ ]
) unprivileged) "unprivileged boot jobs not fully specified";
assert check (
  daemons.sops-install-secrets.serviceConfig.KeepAlive.SuccessfulExit == false
  && daemons.sops-install-secrets.serviceConfig.UserName == "root"
  && daemons.sops-install-secrets.serviceConfig.Umask == 18
) "failed secret provisioning must retry";
assert check (
  daemons.hermes.serviceConfig.EnvironmentVariables.HERMES_CRON_TIMEOUT == "7200"
) "Hermes cron timeout changed";
assert check (
  lib.hasPrefix "/nix/store/" (builtins.head home.dotfiles.darwinDaemons.hermes.ProgramArguments)
  && daemons.hermes.serviceConfig.EnvironmentVariables.HERMES_MANAGED == "home-manager"
  && !(lib.hasInfix "/.hermes/hermes-agent/venv" daemons.hermes.serviceConfig.EnvironmentVariables.PATH)
) "Hermes must use the pinned package, not the imperative checkout";
assert check (
  c.sops.secrets.hermes-declaration.format == "json"
  && c.sops.secrets.hermes-declaration.key == ""
  && c.sops.secrets.hermes-declaration.sopsFile == home.sops.secrets.hermes-declaration.sopsFile
  && home.home.activation ? configureHermesDeclaration
) "Hermes encrypted declaration must reach native secrets and Home Manager";
assert check (lib.hasInfix "chrome-headless-shell" home.home.sessionVariables.CHROME_PATH)
  "browser runtime must be pinned headless shell";
assert check
  (lib.hasInfix "darwin-preflight --target" c.system.activationScripts.preActivation.text)
  "unsafe automatic service takeover";
assert check (lib.hasInfix "darwin-secrets wait" home.home.activation.sops-nix.data)
  "home activation must wait for native secrets";
assert check (home.home.activation ? markDarwinReady) "missing completed-home gate";
assert check (
  c.environment.etc."dotfiles/home-generation".text == "${home.home.activationPackage}\n"
) "system must pin its matching home generation";
assert check (
  c.sops.age.sshKeyPaths == [ "/Users/william/.ssh/id_ed25519" ]
  && !c.sops.age.generateKey
  && c.sops.gnupg.sshKeyPaths == [ ]
) "shared identity changed";
assert check (
  builtins.attrNames c.sops.secrets == builtins.attrNames home.sops.secrets
) "native and home secret groups differ";
assert check (builtins.all (
  name:
  c.sops.secrets.${name}.owner == "william"
  && c.sops.secrets.${name}.mode == "0400"
  && c.sops.secrets.${name}.path == home.sops.secrets.${name}.path
) (builtins.attrNames c.sops.secrets)) "secret ownership or paths changed";
assert check (
  c.sops.templates."hermes.env".path == home.sops.templates."hermes.env".path
  && c.sops.templates."hermes.env".owner == "william"
) "Hermes template mismatch";
assert check (
  daemons.dotfiles-health.serviceConfig.StartInterval == 300
  && daemons.dotfiles-health.serviceConfig.LowPriorityIO
  && !daemons.dotfiles-health.serviceConfig.KeepAlive
) "local monitoring policy";
assert check (!(daemons ? reboot) && c.time.timeZone == null) "reboot or timezone policy changed";
assert check (
  !(lib.hasInfix "AuthorizedKeysFile none" c.services.openssh.extraConfig)
  && !(lib.hasInfix "PasswordAuthentication no" c.services.openssh.extraConfig)
) "SSH access changed";
assert check (
  !c.system.defaults.CustomSystemPreferences."/Library/Preferences/com.apple.SoftwareUpdate".AutomaticallyInstallMacOSUpdates
) "unattended OS upgrades enabled";
assert check (
  home.home.activation.setupLaunchAgents.data == ""
) "graphical launchd activation remains";
assert check (builtins.all (a: !a.enable) (
  builtins.attrValues home.launchd.agents
)) "a user LaunchAgent is still enabled";
assert check (builtins.isString darwin.system.drvPath) "system must evaluate completely";
pkgs.writeText "headless-darwin-contract" "Headless Darwin evaluation assertions passed. Runtime boot tests remain separate.\n"
