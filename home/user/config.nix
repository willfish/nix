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
    ".gitmessage".source = "${configDir}/gitmessage";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".pryrc".source = "${configDir}/pryrc";
  };
}
