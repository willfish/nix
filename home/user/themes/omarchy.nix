{ lib, pkgs }:
let
  source = import ./omarchy-source.nix;
  names = lib.attrNames (import ./palettes.nix);
  wallpaper =
    name:
    let
      directory = "${source}/themes/${name}/backgrounds";
      images = lib.filter (
        file:
        lib.any (extension: lib.hasSuffix extension file) [
          ".webp"
          ".png"
          ".jpg"
          ".jpeg"
        ]
      ) (lib.attrNames (builtins.readDir directory));
      image = "${directory}/${builtins.head images}";
    in
    # Use upstream's first sorted background, as its default picker does.
    # PNG avoids depending on optional WebP loaders in Swaybg.
    pkgs.runCommand "${name}-wallpaper.png" { nativeBuildInputs = [ pkgs.imagemagick ]; } ''
      magick ${lib.escapeShellArg image} PNG:"$out"
    '';
in
{
  wallpapers = lib.genAttrs names wallpaper;
  license = pkgs.writeText "omarchy-LICENSE" (builtins.readFile "${source}/LICENSE");
}
