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
    ".config/tmuxinator".source = "${configDir}/tmuxinator";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".gitmessage".source = "${configDir}/gitmessage";
    ".pryrc".source = "${configDir}/pryrc";
  };
}
