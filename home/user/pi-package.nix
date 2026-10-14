{ callPackage, source }:

let
  upstream = callPackage (source + "/nix/package.nix") { inherit source; };
in
upstream.overrideAttrs (previous: {
  version = "${previous.version}-unstable-${builtins.substring 0 8 source.rev}";
  __intentionallyOverridingVersion = true;
  postInstall = (previous.postInstall or "") + ''
    # Keep the asset paths used by Home Manager and extension consumers stable.
    mkdir -p "$out/libexec"
    ln -s ../lib/pi/node_modules/@earendil-works/pi-coding-agent "$out/libexec/pi"
    wrapProgram "$out/bin/pi" \
      --set PI_PACKAGE_DIR "$out/libexec/pi" \
      --set PI_SKIP_VERSION_CHECK 1 \
      --set PI_TELEMETRY 0
  '';
})
