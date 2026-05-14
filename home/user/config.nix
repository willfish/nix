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

    # Grok CLI global configuration (harness rules + skills from Codex review)
    ".grok/AGENTS.md".source = "${configDir}/grok/AGENTS.md";
    ".grok/skills/create-skill/SKILL.md".source = "${configDir}/grok/skills/create-skill/SKILL.md";
    ".grok/skills/superpowers/SKILL.md".source = "${configDir}/grok/skills/superpowers/SKILL.md";
    ".grok/skills/systematic-debugging/SKILL.md".source = "${configDir}/grok/skills/systematic-debugging/SKILL.md";
    ".grok/skills/verification-before-completion/SKILL.md".source = "${configDir}/grok/skills/verification-before-completion/SKILL.md";
    ".grok/skills/writing-plans/SKILL.md".source = "${configDir}/grok/skills/writing-plans/SKILL.md";

    # High-value reference material (originally from Codex Superpowers + best practices collections)
    # Now living as a timeless reference library under ~/.grok/skills/references/
    ".grok/skills/references/" = {
      source = "${configDir}/grok/skills/references";
      recursive = true;
    };

    # Also deploy the rich AGENTS.md as Claude.md for Claude Code compatibility
    ".claude/CLAUDE.md".source = "${configDir}/grok/AGENTS.md";
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
