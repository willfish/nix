# Package only supported data files, never arbitrary repository contents.
{
  lib,
  pkgs,
  fallbackUnlock,
  fallbackLicense,
}:
theme:
let
  # These themes sort a solid-colour image before their illustrated wallpapers.
  preferredWallpaper =
    {
      community-felix = "01-clouds.png";
      community-midnight = "2-hand-of-adam.png";
      community-oxo-carbon = "BG3.jpg";
      community-robzee84 = "robzee84-wallpaper-2.jpg";
      community-super-game-bro = "02-last-warrior.png";
    }
    .${theme.name} or "";
  wallpaper = lib.findFirst (
    file: builtins.baseNameOf file == preferredWallpaper
  ) (builtins.head theme.backgrounds) theme.backgrounds;
  metadata = pkgs.writeText "${theme.name}-metadata.json" (
    builtins.toJSON {
      inherit (theme)
        name
        label
        nativeMode
        palette
        ;
      backgrounds = map builtins.baseNameOf theme.backgrounds;
    }
  );
  colours = (pkgs.formats.toml { }).generate "${theme.name}-colors.toml" theme.colours;
  copy = destination: file: "install -m 0644 ${lib.escapeShellArg file} \"$out/${destination}/\"";
in
pkgs.runCommand "omarchy-theme-${theme.name}"
  {
    nativeBuildInputs = [ pkgs.imagemagick ];
  }
  ''
    mkdir -p "$out/backgrounds" "$out/licenses"
    install -m 0644 ${metadata} "$out/theme.json"
    install -m 0644 ${colours} "$out/colors.toml"
    ${lib.concatMapStringsSep "\n" (copy "backgrounds") theme.backgrounds}
    ${lib.concatMapStringsSep "\n" (copy "licenses") theme.licenses}
    ${lib.optionalString (theme.btop != null) ''
      install -m 0644 ${lib.escapeShellArg theme.btop} "$out/btop.theme"
    ''}
    ${
      if theme.backgrounds == [ ] then
        ''
          magick -size 1x1 xc:${lib.escapeShellArg "#${theme.palette.base00}"} "$out/wallpaper.png"
        ''
      else
        ''
          magick ${lib.escapeShellArg wallpaper} PNG:"$out/wallpaper.png"
        ''
    }
    ${
      if theme.unlock != null then
        ''
          install -m 0644 ${lib.escapeShellArg theme.unlock} "$out/unlock.png"
        ''
      else
        ''
          install -m 0644 ${fallbackLicense} "$out/licenses/omarchy-LICENSE"
          magick ${fallbackUnlock} -channel RGB +level-colors \
            ${lib.escapeShellArg "#${theme.palette.base05},#${theme.palette.base05}"} "$out/unlock.png"
        ''
    }
  ''
