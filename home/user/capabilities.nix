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
        if [ "$(cat /etc/dotfiles/home-generation 2>/dev/null)" != "$newGenPath" ]; then
          echo "Deploy the matching Darwin system before changing this home generation." >&2
          exit 1
        fi
        /bin/launchctl print system/org.nixos.sops-install-secrets >/dev/null
        ${config.privateConfig.darwinSecretsPackage}/bin/darwin-secrets wait --timeout 0
        if [ -e "$HOME/.config/sops-nix/secrets" ] && [ ! -L "$HOME/.config/sops-nix/secrets" ]; then
          echo "Refusing to replace an unmanaged secret directory; inspect it during handover." >&2
          exit 1
        fi
      ''
    );
    home.activation.invalidateDarwinReady = lib.mkIf cfg.darwinSystemServices (
      lib.hm.dag.entryBetween [ "linkGeneration" "installPackages" ] [ "writeBoundary" ] ''
        rm -f ${lib.escapeShellArg "${config.xdg.stateHome}/dotfiles/home-ready"}
      ''
    );
    home.activation.markDarwinReady = lib.mkIf cfg.darwinSystemServices (
      lib.hm.dag.entryAfter
        (builtins.filter (name: name != "markDarwinReady") (builtins.attrNames config.home.activation))
        ''
          state=${lib.escapeShellArg "${config.xdg.stateHome}/dotfiles"}
          mkdir -p -m 0700 "$state"
          ready=$(mktemp "$state/.home-ready.XXXXXX")
          printf '%s\n' "$newGenPath" > "$ready"
          mv -f "$ready" "$state/home-ready"
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
      desktop = full;
      playwright = full;
      headlessBrowser = interactive;
      localTerminal = interactive && !headlessDarwin;
      development = full;
      documents = interactive;
      nas = cfg.role == "nas";
    };

    # Only declarations change: all hosts retain the shared decryption identity.
    privateConfig.secretGroups = [
      "coding"
      "search"
    ]
    ++ lib.optional cfg.capabilities.work "work"
    ++ lib.optional cfg.capabilities.accounting "accounting"
    ++ lib.optional cfg.capabilities.email "email"
    ++ lib.optional cfg.capabilities.telegram "telegram"
    ++ lib.optional cfg.capabilities.personal "personal"
    ++ lib.optional (cfg.capabilities.nas || full) "nas"
    ++ lib.optional full "legacy";
    # Leave in-flight KB batches alone; the timer picks up the new command next run.
    # Restarting this oneshot makes sd-switch wait for sync and embedding to finish.
    systemd.user.services.knowledge-base =
      lib.mkIf (pkgs.stdenv.isLinux && cfg.capabilities.knowledgeBase)
        {
          Unit."X-RestartIfChanged" = false;
        };

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
