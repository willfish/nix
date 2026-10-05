{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "agent-usage";
  version = "0.1.0";
  src = ../config/agent-usage;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    makeWrapper
  ];
  nativeCheckInputs = with pkgs; [
    nodejs
    openssl
  ];
  buildInputs = with pkgs; [
    yyjson
    curl
    glib
  ];
  doCheck = true;
  postInstall = ''
    for program in "$out/bin/"*; do
      wrapProgram "$program" --set-default SSL_CERT_FILE "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
    done
  '';
  meta.platforms = pkgs.lib.platforms.unix;
}
