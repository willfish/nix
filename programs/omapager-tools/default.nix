{
  pkgs,
  withIcons ? true,
}:
pkgs.stdenv.mkDerivation {
  pname = "omapager-tools";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.glib ] ++ pkgs.lib.optional withIcons pkgs.python3;
  mesonFlags = [ "-Dicons=${pkgs.lib.boolToString withIcons}" ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.linux;
}
