{
  config,
  lib,
  hostName,
  ...
}:
let
  cfg = config.dotfiles;
  # Host identity is resolved here, not repeated throughout consumer modules.
  roles = {
    foundation = "workstation";
    andromeda = "workstation";
    relay = "automation";
    terminus = "nas";
    starfish = "legacy";
  };
  full = builtins.elem cfg.role [
    "workstation"
    "legacy"
  ];
  interactive = cfg.role != "nas";
  capability = description: lib.mkEnableOption description;
in
{
  options.dotfiles = {
    role = lib.mkOption {
      type = lib.types.enum [
        "workstation"
        "automation"
        "nas"
        "legacy"
      ];
      default = roles.${if hostName == null then "" else hostName} or "legacy";
      description = "Host purpose. Legacy preserves unclassified homes until explicitly assigned.";
    };
    capabilities = {
      work = capability "employer integrations and cloud tooling";
      knowledgeBase = capability "work knowledge collection, search and Confluence mirroring";
      accounting = capability "personal accounting and tax workflows";
      email = capability "personal email tooling";
      hermes = capability "Hermes automation scripts and Telegram environment";
      telegram = capability "Telegram agent integration";
      personal = capability "personal desktop apps and interactive account workflows";
      memory = capability "Hindsight memory tooling";
      desktop = capability "desktop applications and desktop settings";
      playwright = capability "visible-browser MCP and Playwright automation";
      development = capability "general development tools (projects still own their dev shells)";
      documents = capability "document generation tools";
      nas = capability "NAS media maintenance tools";
    };
  };

  config = {
    dotfiles.capabilities = lib.mapAttrs (_: lib.mkDefault) {
      work = full;
      knowledgeBase = full;
      accounting = interactive;
      email = interactive;
      hermes = interactive;
      telegram = interactive;
      personal = full;
      memory = full;
      desktop = interactive;
      playwright = interactive;
      development = full;
      documents = interactive;
      nas = cfg.role == "nas";
    };

    # Only declarations change: all hosts retain the shared decryption identity.
    privateConfig.secretGroups = [
      "coding"
    ]
    ++ lib.optional cfg.capabilities.work "work"
    ++ lib.optional cfg.capabilities.accounting "accounting"
    ++ lib.optional cfg.capabilities.email "email"
    ++ lib.optional cfg.capabilities.telegram "telegram"
    ++ lib.optional cfg.capabilities.personal "personal"
    ++ lib.optional cfg.capabilities.memory "memory"
    ++ lib.optional (cfg.capabilities.nas || full) "nas"
    ++ lib.optional full "legacy";
    privateConfig.hermesEnvironment = cfg.capabilities.hermes;

    assertions = [
      {
        assertion = !cfg.capabilities.knowledgeBase || cfg.capabilities.work;
        message = "Knowledge collection requires work integrations.";
      }
      {
        assertion =
          !cfg.capabilities.hermes
          || (cfg.capabilities.telegram && cfg.capabilities.accounting && cfg.capabilities.playwright);
        message = "Hermes automation requires Telegram, accounting and Playwright capabilities.";
      }
    ];
  };
}
