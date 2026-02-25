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
  };
}
