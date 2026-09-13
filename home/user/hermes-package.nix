# Preserve the installed Hermes revision and its upstream Python/npm locks.
# Disable npm lifecycle scripts in the sandbox; explicit UI build commands remain.
{ hermesInput, system }:
let
  pkgs = import hermesInput.inputs.nixpkgs { inherit system; };
  safeCallPackage =
    path: args:
    pkgs.callPackage path (
      args
      // pkgs.lib.optionalAttrs (baseNameOf path == "lib.nix") {
        buildNpmPackage =
          attrs:
          pkgs.buildNpmPackage (
            attrs
            // {
              npm_config_ignore_scripts = "true";
            }
          );
      }
    );
in
(hermesInput.packages.${system}.default.override { callPackage = safeCallPackage; }).overrideAttrs
  (old: {
    passthru = (old.passthru or { }) // {
      sourceRevision = hermesInput.rev;
    };
  })
