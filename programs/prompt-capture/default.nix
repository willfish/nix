{ pkgs }:
let
  # mitmproxy is packaged as an application; mark it as importable for embedding.
  interpreter = pkgs.python3.withPackages (ps: [ (ps.toPythonModule pkgs.mitmproxy) ]);
in
pkgs.stdenv.mkDerivation {
  pname = "prompt-capture-mitm";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ./.;
    fileset = pkgs.lib.fileset.unions [
      ./main.c
      ./meson.build
      ./meson.options
    ];
  };
  nativeBuildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
  ];
  buildInputs = [ pkgs.python3 ];
  mesonFlags = [
    "-Dpython_executable=${interpreter}/bin/python3"
    "-Dsite_packages=${interpreter}/${pkgs.python3.sitePackages}"
  ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
