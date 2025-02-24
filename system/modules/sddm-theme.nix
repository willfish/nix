{ pkgs-unstable }:

pkgs-unstable.stdenv.mkDerivation {
    name = "sddm-theme";
    src = pkgs-unstable.fetchFromGitHub {
        owner = "Keyitdev";
        repo = "sddm-astronaut-theme";
        rev = "5e39e0841d4942757079779b4f0087f921288af6";
        sha256 = "sha256-ew0vPXV7VtjTL3jlyMSobovU3ZNi166XjpO5LHBaNL0=";
    };
    installPhase = ''
        mkdir -p $out
        cp -R ./* $out/
    '';
}
