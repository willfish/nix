{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "git-branches";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
    pkgs.makeWrapper
  ];
  buildInputs = [ pkgs.glib ];
  postInstall = ''
    wrapProgram "$out/bin/git-branches" \
      --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.git ]}
  '';
  doCheck = false;
  meta = {
    description = "Read-only, worktree-aware Git branch overview";
    mainProgram = "git-branches";
    platforms = pkgs.lib.platforms.unix;
  };
}
