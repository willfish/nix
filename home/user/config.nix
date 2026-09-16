{ config, lib, ... }:
let
  configDir = ../config;
in
{
  home.file = {
    ".aprc".source = "${configDir}/aprc";
    ".bin/".source = "${configDir}/bin";
    ".config/nvim/snippets".source = "${configDir}/nvim/snippets";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".gitmessage".source = "${configDir}/gitmessage";
    ".npmrc".source = "${configDir}/npmrc";
    ".pryrc".source = "${configDir}/pryrc";
  };

  home.activation = {
    createDiagramDirectories = lib.mkIf config.dotfiles.capabilities.documents (
      lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        mkdir -p "$HOME/diagrams/generated"
        mkdir -p "$HOME/diagrams/architecture"
      ''
    );

    configureYarn = lib.mkIf config.dotfiles.capabilities.development (
      lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        if [ -L "$HOME/.yarnrc" ]; then
          rm "$HOME/.yarnrc"
        fi
        install -m 0644 ${configDir}/yarnrc "$HOME/.yarnrc"

        if [ -L "$HOME/.yarnrc.yml" ]; then
          rm "$HOME/.yarnrc.yml"
        fi
        install -m 0644 ${configDir}/yarnrc.yml "$HOME/.yarnrc.yml"
      ''
    );
  };
}
