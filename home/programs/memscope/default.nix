{
  lib,
  stdenv,
}:
stdenv.mkDerivation {
  pname = "memscope";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./memscope.h
      ./main.c
      ./proc.c
      ./render.c
    ];
  };
  doCheck = false;
  makeFlags = [ "PREFIX=$(out)" ];
  meta = {
    description = "One-shot RAM overview and proportional mapped-file memory chart";
    platforms = lib.platforms.linux;
    mainProgram = "memscope";
  };
}
