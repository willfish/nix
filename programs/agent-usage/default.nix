{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "agent-usage";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = with pkgs; [
    yyjson
    curl
    glib
  ];
  doCheck = false;
  postInstall = ''
    for program in "$out/bin/"*; do
      wrapProgram "$program" --set-default SSL_CERT_FILE "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
    done
  '';
  meta.platforms = pkgs.lib.platforms.unix;
}
