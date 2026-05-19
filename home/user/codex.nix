{
  config,
  lib,
  pkgs,
  ...
}:
let
  codexConfig = (pkgs.formats.toml { }).generate "codex-config.toml" {
    model = "gpt-5.5";
    model_reasoning_effort = "medium";

    features = {
      goals = true;
    };

    projects = {
      "${config.home.homeDirectory}/.dotfiles" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-backend" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-platform-aws-terraform" = {
        trust_level = "trusted";
      };
    };

    notice.model_migrations = {
      "gpt-5.3-codex" = "gpt-5.4";
    };

    tui.model_availability_nux = {
      "gpt-5.5" = 4;
    };

    plugins = {
      "github@openai-curated" = {
        enabled = true;
      };
    };
  };
in
{
  home.activation.configureCodex = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
    install -d "$HOME/.codex"
    rm -f "$HOME/.codex/config.toml"
    install -m 0600 ${codexConfig} "$HOME/.codex/config.toml"
  '';
}
