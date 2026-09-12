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
    "dotfiles-secrets"
  ];
  check = condition: message: lib.assertMsg condition "Headless Darwin: ${message}";
in
assert check (!c.nix.enable) "must not take ownership of Determinate Nix";
assert check c.services.openssh.enable "native SSH must stay enabled";
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
  daemons.dotfiles-secrets.serviceConfig.KeepAlive.SuccessfulExit == false
) "failed secret provisioning must retry";
assert check (
  daemons.hermes.serviceConfig.EnvironmentVariables.HERMES_CRON_TIMEOUT == "7200"
) "Hermes cron timeout changed";
assert check (lib.hasInfix "chrome-headless-shell" home.home.sessionVariables.CHROME_PATH)
  "browser runtime must be pinned headless shell";
assert check (lib.hasInfix "Refusing handover" c.system.activationScripts.preActivation.text)
  "unsafe automatic service takeover";
assert check (lib.hasInfix "darwin-secrets provision" home.home.activation.sops-nix.data)
  "home activation must provision synchronously";
assert check (
  home.home.activation.setupLaunchAgents.data == ""
) "graphical launchd activation remains";
assert check (builtins.all (a: !a.enable) (
  builtins.attrValues home.launchd.agents
)) "a user LaunchAgent is still enabled";
assert check (builtins.isString darwin.system.drvPath) "system must evaluate completely";
pkgs.writeText "headless-darwin-contract" "Headless Darwin evaluation assertions passed. Runtime boot tests remain separate.\n"
