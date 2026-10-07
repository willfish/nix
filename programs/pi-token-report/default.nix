{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "pi-token-report";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/command-support
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/pi-token-report";
  nativeBuildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
  ];
  buildInputs = [
    pkgs.glib
    pkgs.yyjson
  ];
  # The fixed capture workload asks the nested model to write add.py.
  mesonFlags = [ "-Dprobe_python=${pkgs.python3}/bin/python3" ];
  doCheck = false;
  meta = {
    description = "Generate private Pi token-usage reports from authorized captures";
    mainProgram = "pi-token-report";
    platforms = pkgs.lib.platforms.unix;
  };
}
