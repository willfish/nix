{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  isRelay = pkgs.stdenv.isDarwin && hostName == "relay";
  cfg = config.dotfiles.hermes;
  hermesHome = "${config.home.homeDirectory}/.hermes";
in
{
  options.dotfiles.hermes.declarationFile = lib.mkOption {
    type = lib.types.nullOr lib.types.str;
    default = null;
    description = "Runtime path of the SOPS-decrypted private Hermes declaration.";
  };

  options.dotfiles.hermes.qwenOverlay = lib.mkOption {
    type = lib.types.nullOr lib.types.str;
    default = null;
    description = "Non-secret local-model settings applied before writing the declared Qwen profile.";
  };

  config = lib.mkIf isRelay {
    assertions = [
      {
        assertion = cfg.declarationFile != null;
        message = "Relay requires an encrypted Hermes declaration from the private input.";
      }
    ];
    home.packages = [ pkgs.hermes-agent ];
    home.sessionVariables.HERMES_MANAGED = "home-manager";
    home.file.".hermes/.runtime-revision".text = "${pkgs.hermes-agent.sourceRevision}\n";
    # Override the old imperative launcher, whose path precedes the Nix profile.
    # Home Manager backs up the original during the first migration.
    home.file.".local/bin/hermes".source = "${pkgs.hermes-agent}/bin/hermes";
    home.activation.preserveImperativeHermes = lib.hm.dag.entryBefore [ "checkLinkTargets" ] ''
      launcher=${lib.escapeShellArg "${config.home.homeDirectory}/.local/bin/hermes"}
      if [ -f "$launcher" ] && [ ! -L "$launcher" ]; then
        backup="$launcher.before-home-manager"
        if [ -e "$backup" ]; then
          ${pkgs.diffutils}/bin/cmp -s "$launcher" "$backup" || {
            echo "Hermes launcher backup differs; refusing to overwrite it" >&2
            exit 1
          }
          run ${pkgs.coreutils}/bin/rm "$launcher"
        else
          run ${pkgs.coreutils}/bin/mv "$launcher" "$backup"
        fi
      fi
    '';
    home.activation.configureHermesDeclaration = lib.hm.dag.entryAfter [ "writeBoundary" "sops-nix" ] ''
      PYTHONPATH=${../config/local-llm} \
      ${
        pkgs.python3.withPackages (ps: [ ps.pyyaml ])
      }/bin/python3 ${../config/local-llm/hermes_declaration.py} \
        ${
          lib.escapeShellArg (
            if cfg.declarationFile == null then "/missing-hermes-declaration" else cfg.declarationFile
          )
        } \
        ${lib.escapeShellArg hermesHome} \
        --qwen-overlay ${
          lib.escapeShellArg (if cfg.qwenOverlay == null then "/missing-qwen-overlay" else cfg.qwenOverlay)
        } \
        --key-file ${lib.escapeShellArg "${config.xdg.configHome}/local-llm/api-key"}
    '';
  };
}
