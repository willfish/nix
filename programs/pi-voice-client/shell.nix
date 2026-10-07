{
  pkgs ? import <nixpkgs> { },
}:
pkgs.mkShell {
  packages = with pkgs; [
    gcc
    pkg-config
    yyjson
    curl
    pcre2
    meson
    ninja
    nodejs
    gtk4
    cairo
    pango
    gtk4-layer-shell
  ];
}
