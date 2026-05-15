{ ... }:
let
  configDir = ../config;
in
{
  home.file = {
    ".aprc".source = "${configDir}/aprc";
    ".bin/".source = "${configDir}/bin";
    ".config/ghostty".source = "${configDir}/ghostty";
    ".config/nvim/init.lua".source = "${configDir}/nvim/init.lua";
    ".config/nvim/snippets".source = "${configDir}/nvim/snippets";
    ".config/tmuxinator".source = "${configDir}/tmuxinator";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".gitmessage".source = "${configDir}/gitmessage";
    ".pryrc".source = "${configDir}/pryrc";

    # LLM Harness — single source of truth for Grok CLI, Claude Code, Codex, Gemini CLI (and future TUIs)
    # Universal rules (portable discipline only; no dotfiles/Nix specifics)
    ".grok/AGENTS.md".source = "${configDir}/llm/AGENTS.md";
    ".claude/CLAUDE.md" = {
      source = "${configDir}/llm/AGENTS.md";
      force = true;
    };
    ".codex/AGENTS.md".source = "${configDir}/llm/AGENTS.md";
    ".gemini/GEMINI.md".source = "${configDir}/llm/AGENTS.md";

    # Gemini CLI skills & guides (placeholder — activate when you start using it)
    # Once you know the exact paths Gemini + Superpowers extension expects,
    # we can deploy the job guides + thin skill wrappers the same way we do for Codex/Grok.
    #
    # Example (to be adjusted after first run):
    # ".gemini/skills/hmrc-trade-tariff-workflow/SKILL.md".source = ...;
    # ".gemini/guides/".source = "${configDir}/llm/guides";  # or similar
    # (see llm/gemini/README.md)

    # Job-specific guides (Claude Code "Guides" panel + shared with Codex references)
    ".claude/guides/" = {
      source = "${configDir}/llm/guides";
      recursive = true;
    };

    # Codex custom job-specific skill wrappers (SKILL.md only — references mapped individually below)
    ".codex/skills/hmrc-trade-tariff-workflow/SKILL.md".source = "${configDir}/llm/codex-skills/hmrc-trade-tariff-workflow/SKILL.md";
    ".codex/skills/jira-workflow/SKILL.md".source = "${configDir}/llm/codex-skills/jira-workflow/SKILL.md";
    ".codex/skills/pull-request-workflow/SKILL.md".source = "${configDir}/llm/codex-skills/pull-request-workflow/SKILL.md";
    ".codex/skills/code-review-workflow/SKILL.md".source = "${configDir}/llm/codex-skills/code-review-workflow/SKILL.md";
    ".codex/skills/will-voice/SKILL.md".source = "${configDir}/llm/codex-skills/will-voice/SKILL.md";
    ".codex/skills/daily-notes/SKILL.md".source = "${configDir}/llm/codex-skills/daily-notes/SKILL.md";
    ".codex/skills/rspec-testing/SKILL.md".source = "${configDir}/llm/codex-skills/rspec-testing/SKILL.md";
    ".codex/skills/local-dev-environment/SKILL.md".source = "${configDir}/llm/codex-skills/local-dev-environment/SKILL.md";
    ".codex/skills/chain-of-verification/SKILL.md".source = "${configDir}/llm/codex-skills/chain-of-verification/SKILL.md";
    ".codex/skills/latex-pdfs/SKILL.md".source = "${configDir}/llm/codex-skills/latex-pdfs/SKILL.md";
    ".codex/skills/terminal-demos/SKILL.md".source = "${configDir}/llm/codex-skills/terminal-demos/SKILL.md";

    # Map canonical job guides into each Codex skill's references/ directory (single source of truth, no duplication)
    ".codex/skills/hmrc-trade-tariff-workflow/references/jira.md".source = "${configDir}/llm/guides/jira.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/prs.md".source = "${configDir}/llm/guides/prs.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/epics-and-stories.md".source = "${configDir}/llm/guides/epics-and-stories.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/git.md".source = "${configDir}/llm/guides/git.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/reviews.md".source = "${configDir}/llm/guides/reviews.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/rspec.md".source = "${configDir}/llm/guides/rspec.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/testing.md".source = "${configDir}/llm/guides/testing.md";
    ".codex/skills/hmrc-trade-tariff-workflow/references/trade-tariff-frontend.md".source = "${configDir}/llm/guides/trade-tariff-frontend.md";

    ".codex/skills/jira-workflow/references/jira.md".source = "${configDir}/llm/guides/jira.md";
    ".codex/skills/jira-workflow/references/epics-and-stories.md".source = "${configDir}/llm/guides/epics-and-stories.md";

    ".codex/skills/pull-request-workflow/references/prs.md".source = "${configDir}/llm/guides/prs.md";
    ".codex/skills/pull-request-workflow/references/git.md".source = "${configDir}/llm/guides/git.md";
    ".codex/skills/pull-request-workflow/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".codex/skills/code-review-workflow/references/reviews.md".source = "${configDir}/llm/guides/reviews.md";
    ".codex/skills/code-review-workflow/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".codex/skills/will-voice/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".codex/skills/daily-notes/references/daily-notes.md".source = "${configDir}/llm/guides/daily-notes.md";

    ".codex/skills/rspec-testing/references/rspec.md".source = "${configDir}/llm/guides/rspec.md";
    ".codex/skills/rspec-testing/references/testing.md".source = "${configDir}/llm/guides/testing.md";

    ".codex/skills/chain-of-verification/references/planning.md".source = "${configDir}/llm/guides/planning.md";

    ".codex/skills/latex-pdfs/references/pdfs.md".source = "${configDir}/llm/guides/pdfs.md";

    ".codex/skills/terminal-demos/references/terminal-demos.md".source = "${configDir}/llm/guides/terminal-demos.md";

    # Existing Grok-native process skills and reference library (unchanged)
    ".grok/skills/create-skill/SKILL.md".source = "${configDir}/grok/skills/create-skill/SKILL.md";
    ".grok/skills/superpowers/SKILL.md".source = "${configDir}/grok/skills/superpowers/SKILL.md";
    ".grok/skills/systematic-debugging/SKILL.md".source = "${configDir}/grok/skills/systematic-debugging/SKILL.md";
    ".grok/skills/verification-before-completion/SKILL.md".source = "${configDir}/grok/skills/verification-before-completion/SKILL.md";
    ".grok/skills/writing-plans/SKILL.md".source = "${configDir}/grok/skills/writing-plans/SKILL.md";

    ".grok/skills/references/" = {
      source = "${configDir}/grok/skills/references";
      recursive = true;
    };

    # Job-specific Grok-native skills (thin wrappers; canonical content lives in llm/guides/)
    ".grok/skills/hmrc-trade-tariff-workflow/SKILL.md".source = "${configDir}/grok/skills/hmrc-trade-tariff-workflow/SKILL.md";
    ".grok/skills/jira-workflow/SKILL.md".source = "${configDir}/grok/skills/jira-workflow/SKILL.md";
    ".grok/skills/pull-request-workflow/SKILL.md".source = "${configDir}/grok/skills/pull-request-workflow/SKILL.md";
    ".grok/skills/will-voice/SKILL.md".source = "${configDir}/grok/skills/will-voice/SKILL.md";
    ".grok/skills/code-review-workflow/SKILL.md".source = "${configDir}/grok/skills/code-review-workflow/SKILL.md";
    ".grok/skills/daily-notes/SKILL.md".source = "${configDir}/grok/skills/daily-notes/SKILL.md";
    ".grok/skills/rspec-testing/SKILL.md".source = "${configDir}/grok/skills/rspec-testing/SKILL.md";

    # Map canonical job guides into the Grok skill reference directories
    ".grok/skills/hmrc-trade-tariff-workflow/references/jira.md".source = "${configDir}/llm/guides/jira.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/prs.md".source = "${configDir}/llm/guides/prs.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/epics-and-stories.md".source = "${configDir}/llm/guides/epics-and-stories.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/git.md".source = "${configDir}/llm/guides/git.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/reviews.md".source = "${configDir}/llm/guides/reviews.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/rspec.md".source = "${configDir}/llm/guides/rspec.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/testing.md".source = "${configDir}/llm/guides/testing.md";
    ".grok/skills/hmrc-trade-tariff-workflow/references/trade-tariff-frontend.md".source = "${configDir}/llm/guides/trade-tariff-frontend.md";

    ".grok/skills/jira-workflow/references/jira.md".source = "${configDir}/llm/guides/jira.md";
    ".grok/skills/jira-workflow/references/epics-and-stories.md".source = "${configDir}/llm/guides/epics-and-stories.md";

    ".grok/skills/pull-request-workflow/references/prs.md".source = "${configDir}/llm/guides/prs.md";
    ".grok/skills/pull-request-workflow/references/git.md".source = "${configDir}/llm/guides/git.md";
    ".grok/skills/pull-request-workflow/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".grok/skills/will-voice/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".grok/skills/code-review-workflow/references/reviews.md".source = "${configDir}/llm/guides/reviews.md";
    ".grok/skills/code-review-workflow/references/voice.md".source = "${configDir}/llm/guides/voice.md";

    ".grok/skills/daily-notes/references/daily-notes.md".source = "${configDir}/llm/guides/daily-notes.md";

    ".grok/skills/rspec-testing/references/rspec.md".source = "${configDir}/llm/guides/rspec.md";
    ".grok/skills/rspec-testing/references/testing.md".source = "${configDir}/llm/guides/testing.md";
  };

  xdg.configFile = {
    "cosmic/com.system76.CosmicSettings.Shortcuts/v1/custom" = {
      source = "${configDir}/cosmic/shortcuts";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/autotile" = {
      source = "${configDir}/cosmic/autotile";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/autotile_behavior" = {
      source = "${configDir}/cosmic/autotile_behavior";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/active_hint" = {
      source = "${configDir}/cosmic/active_hint";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/focus_follows_cursor" = {
      source = "${configDir}/cosmic/focus_follows_cursor";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/cursor_follows_focus" = {
      source = "${configDir}/cosmic/cursor_follows_focus";
      force = true;
    };
    "cosmic/com.system76.CosmicPanel.Panel/v1" = {
      source = "${configDir}/cosmic/panel";
      recursive = true;
      force = true;
    };
    "cosmic/com.system76.CosmicPanel.Dock/v1" = {
      source = "${configDir}/cosmic/dock";
      recursive = true;
      force = true;
    };
    "cosmic/com.system76.CosmicPanel/v1/entries" = {
      source = "${configDir}/cosmic/panel-entries";
      force = true;
    };
    "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark" = {
      source = "${configDir}/cosmic/theme-mode";
      force = true;
    };
    "cosmic/com.system76.CosmicTheme.Dark/v1" = {
      source = "${configDir}/cosmic/theme-dark";
      recursive = true;
      force = true;
    };
  };
}
