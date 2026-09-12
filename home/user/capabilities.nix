{
  config,
  lib,
  pkgs,
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
  headlessDarwin = pkgs.stdenv.isDarwin && cfg.role == "automation";
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
    darwinSystemServices = lib.mkOption {
      type = lib.types.bool;
      default = headlessDarwin;
      description = "Use nix-darwin system jobs instead of graphical-session LaunchAgents.";
    };
    darwinDaemons = lib.mkOption {
      type = lib.types.attrsOf lib.types.attrs;
      default = { };
      description = "Unprivileged launchd job specifications consumed by nix-darwin.";
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
      playwright = capability "visible-browser MCP integration";
      headlessBrowser = capability "pinned headless browser automation";
      localTerminal = capability "Ghostty for occasional local maintenance";
      development = capability "general development tools (projects still own their dev shells)";
      documents = capability "document generation tools";
      nas = capability "NAS media maintenance tools";
    };
  };

  config = {
    launchd.enable = lib.mkIf cfg.darwinSystemServices false;
    # This HM revision emits GUI activation even when launchd.enable is false.
    # Old jobs are retired explicitly during handover, not from an SSH switch.
    home.activation.setupLaunchAgents = lib.mkIf cfg.darwinSystemServices (
      lib.mkForce (lib.hm.dag.entryAfter [ "writeBoundary" ] "")
    );
    home.activation.requireDarwinSystem = lib.mkIf cfg.darwinSystemServices (
      lib.hm.dag.entryBefore [ "writeBoundary" ] ''
        if ! /usr/bin/grep -Fq \
          ${lib.escapeShellArg "${config.privateConfig.darwinSecretsPackage}/bin/darwin-secrets"} \
          /Library/LaunchDaemons/org.nixos.dotfiles-secrets.plist; then
          echo "Activate the matching nix-darwin system through the approved handover before this home." >&2
          exit 1
        fi
      ''
    );
    dotfiles.capabilities = lib.mapAttrs (_: lib.mkDefault) {
      work = full;
      knowledgeBase = full;
      accounting = interactive;
      email = interactive;
      hermes = interactive;
      telegram = interactive;
      personal = full;
      memory = full;
      desktop = full;
      playwright = full;
      headlessBrowser = interactive;
      localTerminal = interactive;
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
    privateConfig.darwinSystemService = cfg.darwinSystemServices;

    assertions = [
      {
        assertion = !cfg.capabilities.knowledgeBase || cfg.capabilities.work;
        message = "Knowledge collection requires work integrations.";
      }
      {
        assertion =
          !cfg.capabilities.hermes
          || (cfg.capabilities.telegram && cfg.capabilities.accounting && cfg.capabilities.headlessBrowser);
        message = "Hermes automation requires Telegram, accounting and headless browser capabilities.";
      }
    ];
  };
}
