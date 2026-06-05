{
  lib,
  pkgs,
  ...
}:
let
  inherit (pkgs) stdenv;
  configDir = ../config;
  sourceFile = source: {
    inherit source;
    force = true;
  };
  sourceDir = source: {
    inherit source;
    recursive = true;
    force = true;
  };

  agentRuleFiles = [
    ".grok/AGENTS.md"
    ".claude/CLAUDE.md"
    ".claude/AGENTS.md"
    ".codex/AGENTS.md"
    ".gemini/GEMINI.md"
    ".gemini/AGENTS.md"
    ".agents/AGENTS.md"
    ".agents/GEMINI.md"
    ".antigravity/AGENTS.md"
    ".antigravity/GEMINI.md"
    ".antigravitycli/AGENTS.md"
    ".antigravitycli/GEMINI.md"
  ];

  guideRoots = [
    ".grok/guides"
    ".claude/guides"
    ".codex/guides"
    ".gemini/guides"
    ".agents/guides"
    ".antigravity/guides"
    ".antigravitycli/guides"
  ];

  sharedSkillRoots = [
    ".grok/skills"
    ".claude/skills"
    ".codex/skills"
    ".gemini/skills"
    ".agents/skills"
    ".antigravity/skills"
    ".antigravitycli/skills"
  ];

  processSkillRoots = [
    ".grok/skills"
    ".claude/skills"
    ".gemini/skills"
    ".agents/skills"
    ".antigravity/skills"
    ".antigravitycli/skills"
  ];

  processSkillNames = [
    "create-skill"
    "superpowers"
    "using-superpowers"
    "systematic-debugging"
    "verification-before-completion"
    "writing-plans"
  ];

  sharedSkillNames = [
    "chain-of-verification"
    "code-review-workflow"
    "daily-notes"
    "diagramming"
    "hmrc-trade-tariff-workflow"
    "javascript-supply-chain-security"
    "jira-workflow"
    "latex-pdfs"
    "local-dev-environment"
    "pull-request-workflow"
    "rspec-testing"
    "terminal-demos"
    "tui-design"
    "will-voice"
  ];

  sharedSkillReferences = {
    chain-of-verification = {
      "planning.md" = "${configDir}/llm/guides/planning.md";
    };
    code-review-workflow = {
      "reviews.md" = "${configDir}/llm/guides/reviews.md";
      "voice.md" = "${configDir}/llm/guides/voice.md";
    };
    daily-notes = {
      "daily-notes.md" = "${configDir}/llm/guides/daily-notes.md";
    };
    diagramming = {
      "diagramming.md" = "${configDir}/llm/guides/diagramming.md";
      "diagram-review-checklist.md" =
        "${configDir}/llm/skills/diagramming/references/diagram-review-checklist.md";
      "diagramming-how-to.md" = "${configDir}/llm/guides/diagramming-how-to.md";
      "mermaid-github-tips.md" = "${configDir}/llm/skills/diagramming/references/mermaid-github-tips.md";
      "rendering-diagrams.md" = "${configDir}/llm/skills/diagramming/references/rendering-diagrams.md";
      "tool-selection.md" = "${configDir}/llm/skills/diagramming/references/tool-selection.md";
    };
    hmrc-trade-tariff-workflow = {
      "jira.md" = "${configDir}/llm/guides/jira.md";
      "prs.md" = "${configDir}/llm/guides/prs.md";
      "epics-and-stories.md" = "${configDir}/llm/guides/epics-and-stories.md";
      "git.md" = "${configDir}/llm/guides/git.md";
      "reviews.md" = "${configDir}/llm/guides/reviews.md";
      "rspec.md" = "${configDir}/llm/guides/rspec.md";
      "testing.md" = "${configDir}/llm/guides/testing.md";
      "trade-tariff-frontend.md" = "${configDir}/llm/guides/trade-tariff-frontend.md";
    };
    jira-workflow = {
      "jira.md" = "${configDir}/llm/guides/jira.md";
      "epics-and-stories.md" = "${configDir}/llm/guides/epics-and-stories.md";
    };
    javascript-supply-chain-security = {
      "javascript-supply-chain-security.md" =
        "${configDir}/llm/guides/javascript-supply-chain-security.md";
    };
    latex-pdfs = {
      "pdfs.md" = "${configDir}/llm/guides/pdfs.md";
    };
    local-dev-environment = { };
    pull-request-workflow = {
      "prs.md" = "${configDir}/llm/guides/prs.md";
      "git.md" = "${configDir}/llm/guides/git.md";
      "voice.md" = "${configDir}/llm/guides/voice.md";
    };
    rspec-testing = {
      "rspec.md" = "${configDir}/llm/guides/rspec.md";
      "testing.md" = "${configDir}/llm/guides/testing.md";
    };
    terminal-demos = {
      "terminal-demos.md" = "${configDir}/llm/guides/terminal-demos.md";
    };
    tui-design = { };
    will-voice = {
      "voice.md" = "${configDir}/llm/guides/voice.md";
    };
  };

  mkAgentRuleFiles = lib.genAttrs agentRuleFiles (_: sourceFile "${configDir}/llm/AGENTS.md");
  mkGuideFiles = lib.genAttrs guideRoots (_: sourceDir "${configDir}/llm/guides");

  mkProcessSkillFiles =
    root:
    lib.genAttrs (map (skill: "${root}/${skill}/SKILL.md") processSkillNames) (
      target:
      sourceFile "${configDir}/llm/process-skills/${builtins.elemAt (lib.splitString "/" target) 2}/SKILL.md"
    );

  mkReferenceLibraryFiles = root: {
    "${root}/references" = sourceDir "${configDir}/llm/references";
  };

  mkSharedSkillFiles =
    root:
    lib.listToAttrs (
      lib.concatMap (
        skill:
        [
          {
            name = "${root}/${skill}/SKILL.md";
            value = sourceFile "${configDir}/llm/skills/${skill}/SKILL.md";
          }
        ]
        ++ lib.mapAttrsToList (referenceName: referenceSource: {
          name = "${root}/${skill}/references/${referenceName}";
          value = sourceFile referenceSource;
        }) sharedSkillReferences.${skill}
      ) sharedSkillNames
    );
in
{
  home.file = {
    ".aprc".source = "${configDir}/aprc";
    ".bin/".source = "${configDir}/bin";
    ".config/ghostty".source = "${configDir}/ghostty";
    ".config/nvim/snippets".source = "${configDir}/nvim/snippets";
    ".config/tmuxinator".source = "${configDir}/tmuxinator";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".gitmessage".source = "${configDir}/gitmessage";
    ".npmrc".source = "${configDir}/npmrc";
    ".pryrc".source = "${configDir}/pryrc";
    ".yarnrc" = {
      source = "${configDir}/yarnrc";
      force = true;
    };
    ".yarnrc.yml" = {
      source = "${configDir}/yarnrc.yml";
      force = true;
    };
  }
  // lib.optionalAttrs stdenv.isDarwin {
    ".aerospace.toml" = sourceFile "${configDir}/aerospace/aerospace.toml";
  }
  // mkAgentRuleFiles
  // mkGuideFiles
  // lib.foldl' (acc: root: acc // mkSharedSkillFiles root) { } sharedSkillRoots
  // lib.foldl' (acc: root: acc // mkProcessSkillFiles root) { } processSkillRoots
  // lib.foldl' (acc: root: acc // mkReferenceLibraryFiles root) { } processSkillRoots;

  home.activation = {
    createDiagramDirectories = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      mkdir -p "$HOME/diagrams/generated"
      mkdir -p "$HOME/diagrams/architecture"
    '';

    # cosmic-screenshot crashes on launch when CosmicPortal remembers Window mode
    # (NixOS/nixpkgs#409441). Reset only that broken persisted choice.
    fixCosmicScreenshotPortalConfig = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      portalScreenshot="$HOME/.config/cosmic/com.system76.CosmicPortal/v1/screenshot"
      if [ -f "$portalScreenshot" ] && grep -q 'choice: Window' "$portalScreenshot"; then
        ${pkgs.gnused}/bin/sed -i 's/choice: Window/choice: Rectangle/' "$portalScreenshot"
        echo "Reset cosmic screenshot mode from Window to Rectangle (avoids crash loop)"
      elif [ ! -f "$portalScreenshot" ]; then
        mkdir -p "$(dirname "$portalScreenshot")"
        cp ${configDir}/cosmic/portal-screenshot "$portalScreenshot"
        echo "Installed default cosmic screenshot portal config"
      fi
    '';
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
