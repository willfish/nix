# Immutable Omarchy SDDM and Plymouth packages from the pin used by
# home/user/themes/omarchy.nix. The caller selects the boot palette. Built
# packages contain only store paths; they do not read home or user files.
{
  lib,
  pkgs,
}:
let
  source = import ../../home/user/themes/omarchy-source.nix;
  catalogue = import ../../home/user/themes/palettes.nix;
  themes = import ../../home/user/themes/omarchy.nix { inherit lib pkgs; };
  configuredAppearance = (import ../../home/config/hyprland/settings.nix).appearance;
  sddmSource = "${source}/default/sddm/omarchy";
  plymouthSource = "${source}/default/plymouth";
  license = "${source}/LICENSE";
  failedHex = "f7768e";

  applyOverrides =
    theme:
    let
      mode = theme.nativeMode;
      overrides = configuredAppearance.paletteOverrides.${mode} or { };
      base = theme.${mode};
      valid = lib.all (
        key:
        builtins.hasAttr key base
        && builtins.isString overrides.${key}
        && builtins.match "[0-9a-fA-F]{6}" overrides.${key} != null
      ) (lib.attrNames overrides);
    in
    assert lib.assertMsg (
      mode == "dark" || mode == "light"
    ) "theme ${theme.name} nativeMode must be dark or light";
    assert lib.assertMsg valid
      "paletteOverrides.${mode} must contain Base16 keys and six-digit hex colours";
    base // overrides;

  hexByte = hex: offset: (builtins.fromTOML "value = 0x${builtins.substring offset 2 hex}").value;
  # Match awk `printf "%.3f", n/255` from bin/omarchy-plymouth-set. An exact
  # half cannot occur for an integer channel, so half-even does not matter.
  channel =
    n:
    let
      scaled = n * 1000;
      quot = builtins.div scaled 255;
      rem = scaled - quot * 255;
      milli = if rem * 2 >= 255 then quot + 1 else quot;
      whole = builtins.div milli 1000;
      frac = milli - whole * 1000;
      digits = toString frac;
      pad =
        if frac < 10 then
          "00"
        else if frac < 100 then
          "0"
        else
          "";
    in
    "${toString whole}.${pad}${digits}";
  rgbOf = hex: "${channel (hexByte hex 0)}, ${channel (hexByte hex 2)}, ${channel (hexByte hex 4)}";

  replacePrefixedLine =
    text: prefix: replacement:
    let
      lines = lib.splitString "\n" text;
      found = lib.any (line: lib.hasPrefix prefix line) lines;
    in
    assert lib.assertMsg found "missing ${prefix}";
    lib.concatStringsSep "\n" (
      map (line: if lib.hasPrefix prefix line then replacement else line) lines
    );

  inherit (configuredAppearance) monoFont;
  trim = line: lib.removePrefix (builtins.head (builtins.match "( *).*" line)) line;
  findIndex =
    pred: list:
    let
      go =
        i:
        if i >= builtins.length list then
          null
        else if pred (builtins.elemAt list i) then
          i
        else
          go (i + 1);
    in
    go 0;
  splice =
    lines: start: count: replacement:
    lib.sublist 0 start lines
    ++ replacement
    ++ lib.sublist (start + count) (builtins.length lines) lines;
  # Ordinary strings keep the upstream two-space indent. Indented Nix strings
  # would strip it and miss the source block.
  # The upstream layout has no user chooser; never strand the login form on
  # an unrelated last user. Password authentication remains unchanged.
  userBlock = [ ''property string currentUser: "william"'' ];
  sessionBlock = [
    "  property int sessionIndex: {"
    "    // SDDM SessionModel::FileRole, not Qt.DisplayRole (which is empty)."
    "    // FileRole returns an absolute path. Match its exact basename."
    "    var fileRole = Qt.UserRole + 2"
    "    for (var i = 0; i < sessionModel.rowCount(); i++) {"
    "      var file = sessionModel.data(sessionModel.index(i, 0), fileRole)"
    "      if (typeof file === \"string\" && file.split(\"/\").pop() === \"hyprland.desktop\")"
    "        return i"
    "    }"
    "    return -1"
    "  }"
  ];
  qmlStructure =
    let
      raw = builtins.readFile "${sddmSource}/Main.qml";
      lines = lib.splitString "\n" raw;
      userAt = findIndex (line: trim line == "property string currentUser: userModel.lastUser") lines;
      sessionAt = findIndex (line: trim line == "property int sessionIndex: {") lines;
      lastAt = findIndex (line: trim line == "return sessionModel.lastIndex") lines;
      withUser = splice lines userAt 1 userBlock;
      withSession = splice withUser (sessionAt + (builtins.length userBlock - 1)) (
        lastAt - sessionAt + 2
      ) sessionBlock;
      withLogin =
        lib.replaceStrings
          [
            "Keys.onPressed: {"
            "sddm.login(root.currentUser, password.text, root.sessionIndex)"
            "root.loginFailed = true\n      password.text = \"\""
          ]
          [
            "Keys.onPressed: function(event) {"
            "if (root.sessionIndex >= 0) sddm.login(root.currentUser, password.text, root.sessionIndex)"
            "password.text = \"\"\n      root.loginFailed = true"
          ]
          (lib.concatStringsSep "\n" withSession);
      withFont =
        lib.replaceStrings [ ''"JetBrainsMono Nerd Font"'' ] [ (builtins.toJSON monoFont) ]
          withLogin;
    in
    assert lib.assertMsg (
      userAt != null && sessionAt != null && lastAt != null && lastAt > sessionAt
    ) "upstream SDDM session block was not found";
    assert lib.assertMsg (lib.hasInfix "uwsm" raw) "upstream SDDM theme no longer selects uwsm";
    assert lib.assertMsg (lib.hasInfix "font.family: ${builtins.toJSON monoFont}" withFont)
      "SDDM QML font was not patched";
    assert lib.assertMsg (
      !lib.hasInfix "uwsm" withFont && !lib.hasInfix "lastIndex" withFont
    ) "SDDM QML still prefers uwsm or the last session";
    withFont;
  colorize =
    background: foreground:
    let
      step1 = lib.replaceStrings [ "#1a1b26" ] [ "#__OMARCHY_SDDM_BG__" ] qmlStructure;
      step2 = lib.replaceStrings [ "#ffffff" ] [ "#__OMARCHY_SDDM_TEXT__" ] step1;
      step3 = lib.replaceStrings [ "#__OMARCHY_SDDM_BG__" ] [ "#${background}" ] step2;
      step4 = lib.replaceStrings [ "#__OMARCHY_SDDM_TEXT__" ] [ "#${foreground}" ] step3;
    in
    assert lib.assertMsg (lib.hasInfix "color: \"#${background}\"" step4)
      "SDDM background colour was not applied";
    assert lib.assertMsg (!lib.hasInfix "__OMARCHY_SDDM_" step4) "SDDM colour placeholder remained";
    step4;

  patchScript =
    rgb:
    replacePrefixedLine (replacePrefixedLine (builtins.readFile "${plymouthSource}/omarchy.script")
      "Window.SetBackgroundTopColor"
      "Window.SetBackgroundTopColor(${rgb});"
    ) "Window.SetBackgroundBottomColor" "Window.SetBackgroundBottomColor(${rgb});";

  metadataText =
    let
      raw = builtins.readFile "${sddmSource}/metadata.desktop";
      lines = lib.splitString "\n" raw;
      hasVersion = lib.any (line: lib.hasPrefix "QtVersion=" line) lines;
      replaced = map (line: if lib.hasPrefix "QtVersion=" line then "QtVersion=6" else line) lines;
      text = if hasVersion then lib.concatStringsSep "\n" replaced else raw + "QtVersion=6\n";
    in
    assert lib.assertMsg (lib.hasInfix "QtVersion=6" text) "SDDM metadata must declare QtVersion=6";
    text;
  metadataFile = pkgs.writeText "omarchy-sddm-metadata.desktop" metadataText;

  packageFor =
    name: theme:
    let
      palette = applyOverrides theme;
      background = palette.base00;
      foreground = palette.base05;
      rgb = rgbOf background;
      qmlFile = pkgs.writeText "omarchy-${name}-Main.qml" (colorize background foreground);
      scriptFile = pkgs.writeText "omarchy-${name}.script" (patchScript rgb);
    in
    assert lib.assertMsg (builtins.match "[0-9a-fA-F]{6}" background != null) "invalid background";
    assert lib.assertMsg (builtins.match "[0-9a-fA-F]{6}" foreground != null) "invalid foreground";
    pkgs.runCommandLocal "omarchy-greeter-${name}"
      {
        nativeBuildInputs = [ pkgs.imagemagick ];
        omarchyForeground = foreground;
      }
      ''
        set -euo pipefail
        sddm=$out/share/sddm/themes/omarchy
        ply=$out/share/plymouth/themes/omarchy
        mkdir -p "$sddm" "$ply"

        recolor() {
          magick "$1" -channel RGB +level-colors "#$omarchyForeground","#$omarchyForeground" "$2"
        }

        recolor ${plymouthSource}/bullet.png "$ply/bullet.png"
        recolor ${plymouthSource}/entry.png "$ply/entry.png"
        recolor ${plymouthSource}/lock.png "$ply/lock.png"
        recolor ${plymouthSource}/progress_bar.png "$ply/progress_bar.png"
        install -m 0644 ${plymouthSource}/progress_box.png "$ply/progress_box.png"
        install -m 0644 ${themes.packages.${name}}/unlock.png "$ply/logo.png"

        install -m 0644 "$ply/bullet.png" "$sddm/bullet.png"
        install -m 0644 "$ply/entry.png" "$sddm/entry.png"
        install -m 0644 "$ply/lock.png" "$sddm/lock.png"
        install -m 0644 "$ply/logo.png" "$sddm/logo.png"
        magick "$ply/entry.png" -channel RGB +level-colors "#${failedHex}","#${failedHex}" "$sddm/entry-failed.png"
        magick "$ply/lock.png" -channel RGB +level-colors "#${failedHex}","#${failedHex}" "$sddm/lock-failed.png"

        install -m 0644 ${qmlFile} "$sddm/Main.qml"
        install -m 0644 ${metadataFile} "$sddm/metadata.desktop"
        install -m 0644 ${sddmSource}/theme.conf "$sddm/theme.conf"
        install -m 0644 ${scriptFile} "$ply/omarchy.script"
        install -m 0644 ${license} "$sddm/LICENSE"
        install -m 0644 ${license} "$ply/LICENSE"

        sed \
          -e "s|^ImageDir=.*|ImageDir=$ply|" \
          -e "s|^ScriptFile=.*|ScriptFile=$ply/omarchy.script|" \
          ${plymouthSource}/omarchy.plymouth > "$ply/omarchy.plymouth"
      '';

  packages = lib.mapAttrs packageFor catalogue;
in
{
  inherit catalogue configuredAppearance;
  names = lib.attrNames catalogue;
  nativeMode = name: catalogue.${name}.nativeMode;
  sddmPackage = name: packages.${name};
  plymouthPackage = name: packages.${name};
  greeterThemePath = name: "${packages.${name}}/share/sddm/themes/omarchy";
}
