{ pkgs-unstable }:

pkgs-unstable.stdenv.mkDerivation {
    name = "sddm-theme";
    src = pkgs-unstable.fetchFromGitHub {
        owner = "Keyitdev";
        repo = "sddm-astronaut-theme";
        rev = "9048065f6d93abbfb57102a0d8ca278f15e877d3";
        sha256 = "sha256-ew0vPXV7VtjTL3jlyMSobovU3ZNi166XjpO5LHBaNL0=";
    };
    installPhase = ''
        mkdir -p $out
        cp -R ./* $out/
    '';
}
