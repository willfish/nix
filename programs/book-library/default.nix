{ pkgs }:
let
  ocr = pkgs.tesseract.override { enableLanguages = [ "eng" ]; };
  runtime = with pkgs; [
    coreutils
    util-linux
    kiwix-tools
    zim-tools
    poppler-utils
    ocr
  ];
in
pkgs.rustPlatform.buildRustPackage {
  pname = "book-library";
  version = "0.1.0";
  src = ./.;
  cargoLock.lockFile = ./Cargo.lock;
  nativeBuildInputs = [ pkgs.makeWrapper ];
  doCheck = false;
  postInstall = ''
    wrapProgram $out/bin/book-library --prefix PATH : ${pkgs.lib.makeBinPath runtime}
  '';
  meta = {
    description = "Read-only offline book and reference retrieval with explicit corpus maintenance";
    mainProgram = "book-library";
    platforms = pkgs.lib.platforms.linux;
  };
}
