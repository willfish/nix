{
  lib,
  pkgs,
  homes,
}:
let
  expected = rec {
    "william@relay" = william-darwin;
    william = {
      role = "legacy";
      secrets = 56;
    };
    william-linux = {
      role = "legacy";
      secrets = 56;
    };
    william-darwin = {
      role = "automation";
      secrets = 34;
    };
    "william@foundation" = {
      role = "workstation";
      secrets = 56;
    };
    "william@andromeda" = {
      role = "workstation";
      secrets = 56;
    };
    "william@starfish" = {
      role = "legacy";
      secrets = 56;
    };
    "william@terminus" = {
      role = "nas";
      secrets = 14;
    };
  };
  inspect =
    name: spec:
    let
      c = homes.${name}.config;
      caps = c.dotfiles.capabilities;
      packageNames = map lib.getName c.home.packages;
      servers = (builtins.fromJSON c.home.file.".config/mcp/mcp.json".text).mcpServers;
      catalogue = (import ../home/user/llm-mcps.nix { config = c; }).servers;
      has = path: builtins.hasAttr path c.home.file;
      require = condition: message: lib.assertMsg condition "${name}: ${message}";
      workSkills = c.privateConfig.llm.capabilities.work or [ ];
      accountingSkills = c.privateConfig.llm.capabilities.accounting or [ ];
      overlayTimers = c.privateConfig.overlayTimers or [ ];
      hermesActivations = c.privateConfig.hermesActivations or [ ];
    in
    assert require (c.dotfiles.role == spec.role) "wrong role";
    assert require (
      builtins.length (builtins.attrNames c.sops.secrets) == spec.secrets
    ) "wrong secret declarations";
    assert require (
      builtins.attrNames servers == builtins.sort (a: b: a < b) (map (s: s.name) catalogue)
    ) "wrong MCP registrations";
    assert require (builtins.all (
      s: has ".local/bin/${s.wrapper}"
    ) catalogue) "registered MCP wrapper missing";
    assert require (
      builtins.elem "pi" packageNames && builtins.elem "agent-browser" packageNames
    ) "Pi and agent-browser must be everywhere";
    assert require (
      c.sops.secrets ? PI_OPENAI_CODEX_REFRESH
      && c.sops.secrets ? PI_AGENT_BUS_TOKEN
      && c.sops.secrets ? OPENROUTER_API_KEY
      && c.sops.secrets ? GPG_SIGNING_PRIVATE_KEY
      && c.sops.secrets ? LOCAL_LLM_RELAY_API_KEY
      && c.sops.secrets ? LOCAL_LLM_ANDROMEDA_API_KEY
      && c.sops.secrets ? TYPESAFE_API_KEY
    ) "Pi/Git credentials missing";
    assert require (
      has ".agents/skills/systematic-debugging" && has ".agents/skills/local-dev-environment"
    ) "common engineering skills missing";
    assert require (
      workSkills == [ ] || has ".agents/skills/${builtins.head workSkills}" == caps.work
    ) "work skills escaped capability";
    assert require (
      accountingSkills == [ ] || has ".agents/skills/${builtins.head accountingSkills}" == caps.accounting
    ) "accounting skill boundary";
    assert require (has ".agents/skills/daily-notes" == caps.knowledgeBase) "knowledge skill boundary";
    assert require (
      (c.home.activation ? createKnowledgeBaseDirs) == caps.knowledgeBase
    ) "knowledge activation boundary";
    assert require (
      (c.systemd.user.services ? knowledge-base)
      == (homes.${name}.pkgs.stdenv.isLinux && caps.knowledgeBase)
    ) "knowledge service boundary";
    assert require (
      !(c.systemd.user.services ? knowledge-base)
      || (
        (c.systemd.user.services.knowledge-base.Unit."X-RestartIfChanged" or true) == false
        && c.systemd.user.services.knowledge-base.Service.Type == "oneshot"
        && c.systemd.user.timers.knowledge-base.Timer.Unit == "knowledge-base.service"
      )
    ) "knowledge batch must remain timer-driven without restarting on switches";
    assert require (builtins.all (
      key: (builtins.hasAttr key c.home.activation) == caps.hermes
    ) hermesActivations) "Hermes activation boundary";
    assert require ((c.sops.templates ? "hermes.env") == caps.hermes) "Hermes secret template boundary";
    assert require (
      caps.work || !(builtins.elem "work" c.privateConfig.secretGroups)
    ) "work secrets on non-work host";
    assert require (
      caps.work
      || builtins.all (p: !(builtins.elem p packageNames)) [
        "awscli2"
        "terraform"
        "slack"
        "valkey"
        "postgresql"
        "ecs"
      ]
    ) "work packages on non-work host";
    assert require (
      caps.knowledgeBase
      || (
        !(c.systemd.user.timers ? knowledge-base)
        && builtins.all (t: !(builtins.hasAttr t c.systemd.user.timers)) overlayTimers
      )
    ) "work timers on non-work host";
    assert require (
      caps.work || (!(has ".local/bin/mcp-slack") && !(has ".local/bin/slack-refresh-session"))
    ) "disabled work wrappers remain";
    assert require (!(has ".local/bin/mcp-brave")) "Playwright MCP wrapper is not installed";
    assert require (
      !(builtins.elem "playwright-mcp" packageNames)
    ) "Playwright MCP package is not installed";
    assert require (
      caps.playwright || !(has ".local/bin/mcp-agent-browser")
    ) "visible browser wrappers on headless host";
    assert require (has ".local/bin/mcp-dap" == caps.development) "debugger wrapper boundary";
    assert require (
      !(builtins.elem name [
        "william-darwin"
        "william@relay"
      ])
      || (
        !caps.desktop
        && !caps.playwright
        && caps.headlessBrowser
        && !caps.localTerminal
        && !(builtins.elem "ghostty-bin" packageNames)
        && !(c.home.sessionVariables ? TERMINAL)
        && builtins.all (p: !(builtins.elem p packageNames)) [
          "brave"
          "telegram-desktop"
        ]
        && !(has ".config/ghostty")
        && !(c.home.activation ? fixDarwinBraveSignature)
        && !(c.home.activation ? relayServerTrim)
        && !(c.home.sessionVariables ? BROWSER)
        && !c.launchd.enable
        && c.privateConfig.darwinSystemService
        && (c.home.activation ? requireDarwinSystem)
        && c.dotfiles.darwinDaemons ? hermes
        && c.dotfiles.darwinDaemons ? local-llm
        && c.dotfiles.darwinDaemons ? local-assistant-tools
      )
    ) "headless Darwin contract (no local terminal dependency)";
    assert require (
      spec.role != "nas"
      || (
        !c.dconf.enable && !(c.home.activation ? dconfSettings) && !(c.home.activation ? stylixLookAndFeel)
      )
    ) "desktop activation on NAS";
    assert require (
      spec.role != "nas"
      || builtins.all (p: !(builtins.elem p packageNames)) [
        "playwright-mcp"
        "mcp-dap-server"
        "telegram-mcp"
        "himalaya"
        "pandoc"
        "go"
        "ruby"
        "mpv"
        "totp-from-sops"
        "knowledge-base"
      ]
    ) "non-maintenance tools on NAS";
    {
      inherit (spec) role secrets;
      packages = packageNames;
      servers = builtins.attrNames servers;
    };
  report = lib.mapAttrs inspect expected;
in
pkgs.writeText "host-capability-boundaries.json" (builtins.toJSON report)
