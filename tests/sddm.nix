# Run with: direnv exec . nix build .#checks.x86_64-linux.sddm --no-link -L
{
  pkgs ? import (builtins.getFlake (toString ../.)).inputs.nixpkgs {
    system = builtins.currentSystem;
  },
}:
let
  inherit (pkgs) lib;
  rendered = import ../system/modules/greeter-themes.nix { inherit lib pkgs; };
  themes = pkgs.writeText "sddm-test-themes.json" (
    builtins.toJSON (lib.genAttrs rendered.names rendered.greeterThemePath)
  );
  python = pkgs.python3.withPackages (p: [ p.pyside6 ]);
in
pkgs.runCommand "sddm-qml-regressions" { nativeBuildInputs = [ python ]; } ''
  export HOME=$TMPDIR/home
  mkdir -p "$HOME"
  export QT_QPA_PLATFORM=offscreen
  export QT_QUICK_BACKEND=software
  export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }}
  export XDG_RUNTIME_DIR=$TMPDIR/runtime
  mkdir -m 700 "$XDG_RUNTIME_DIR"
  export QML_IMPORT_PATH=${pkgs.kdePackages.sddm-unwrapped}/lib/qt-6/qml
  python ${./sddm_qml.py} ${themes} ${pkgs.kdePackages.sddm-unwrapped.src}
  touch "$out"
''
