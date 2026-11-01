{
  config,
  lib,
  pkgs,
  ...
}:
let
  dictionary = pkgs.hunspellDicts.en_GB-ise;
  filterUrls = lib.unique (
    lib.concatMap (group: group.sources) (
      builtins.fromJSON (builtins.readFile ../config/nyxt/filter-lists.json)
    )
  );
  filterLists = lib.concatMapStringsSep "\n" (
    url:
    ''("brave-${
      builtins.substring 0 16 (builtins.hashString "sha256" url)
    }.txt" ${builtins.toJSON url})''
  ) filterUrls;
  nyxt =
    pkgs.runCommand "nyxt-profile-${pkgs.nyxt.version}"
      {
        nativeBuildInputs = [ pkgs.makeWrapper ];
        inherit (pkgs.nyxt) meta;
      }
      ''
        mkdir -p "$out/bin"
        makeWrapper ${lib.getExe pkgs.nyxt} "$out/bin/nyxt" \
          --prefix DICPATH : ${dictionary}/share/hunspell
        ln -s ${pkgs.nyxt}/share "$out/share"
      '';
in
{
  config = lib.mkIf (pkgs.stdenv.isLinux && config.dotfiles.capabilities.desktop) {
    home.packages = [ nyxt ];
    xdg.configFile."nyxt/config.lisp".source = pkgs.replaceVars ../config/nyxt/config.lisp {
      inherit filterLists;
    };
  };
}
