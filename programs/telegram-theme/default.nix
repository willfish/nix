{ pkgs }:
{ palette, background }:
assert builtins.match "[0-9a-fA-F]{6}" background != null;
pkgs.runCommand "telegram.tdesktop-theme"
  {
    nativeBuildInputs = [
      pkgs.imagemagick
      pkgs.zip
    ];
  }
  ''
    cp ${builtins.toFile "telegram.tdesktop-palette" palette} colors.tdesktop-theme
    magick -size 1x1 ${pkgs.lib.escapeShellArg "xc:#${background}"} -strip PNG24:background.png
    touch -t 198001010000 colors.tdesktop-theme background.png
    zip -X -q - colors.tdesktop-theme background.png > "$out"
  ''
