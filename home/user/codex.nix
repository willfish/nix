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
      "${config.home.homeDirectory}/Repositories/hmrc/electronic-tariff-file" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/fpo_categorisation_prototype_ui" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/identity" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/llm-commodity-classification" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/process-appendix-5a" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/routing-filter" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-admin" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-api-docs" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-backend" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-classification-examples" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-commodi-tea" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-dev-hub" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-e2e-tests" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-fpo-dev-hub-e2e" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-frontend" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-lambdas-authenticator" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-lambdas-database-backups" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-lambdas-electronic-tariff-file-rotations" =
        {
          trust_level = "trusted";
        };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-lambdas-fpo-search" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-platform-aws-terraform" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-platform-terraform-aws-accounts" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-platform-terraform-modules" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-reporting" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-team" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-tech-docs" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/trade-tariff-tools" = {
        trust_level = "trusted";
      };
      "${config.home.homeDirectory}/Repositories/hmrc/uktt" = {
        trust_level = "trusted";
      };
    };

    notice.model_migrations = {
      "gpt-5.3-codex" = "gpt-5.4";
    };

    tui = {
      status_line = [
        "model-with-reasoning"
        "current-dir"
        "git-branch"
        "pull-request-number"
        "context-remaining"
        "five-hour-limit"
        "used-tokens"
      ];
      status_line_use_colors = true;

      model_availability_nux = {
        "gpt-5.5" = 4;
      };
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
