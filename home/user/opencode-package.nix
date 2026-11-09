{
  pkgs,
  source,
  bun,
}:
let
  runtimeLibraries = pkgs.symlinkJoin {
    name = "opencode-runtime-libraries";
    paths =
      map pkgs.lib.getLib [
        pkgs.wayland
        pkgs.libxcb
      ]
      ++ [ pkgs.stdenv.cc.cc.lib ];
  };
  nodeModules = pkgs.callPackage "${source}/nix/node_modules.nix" (
    {
      inherit bun;
      rev = source.shortRev;
    }
    // pkgs.lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "x86_64-linux") {
      hash = "sha256-hV75d+Zs51vB6MfZ9luH1JB6jO+tqp9RYqqYyi3zQM4=";
    }
    // pkgs.lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "aarch64-darwin") {
      hash = "sha256-bp2jsa4iX5jX4O3vWQC9TRSwsAtnkzXX86O90NaJdq8=";
    }
  );
  upstream = pkgs.callPackage "${source}/nix/opencode.nix" (
    {
      inherit bun;
      node_modules = nodeModules;
    }
    // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
      # Upstream's wrapper exposes only a Wayland argument; keep the release
      # package's complete dlopen closure for the watcher and clipboard too.
      wayland = runtimeLibraries;
    }
  );
in
upstream.overrideAttrs (old: {
  __intentionallyOverridingVersion = true;
  version = "${old.version}.mcp.${
    builtins.substring 0 8 (builtins.hashFile "sha256" ./opencode-packages/deferred-mcp.patch)
  }";
  patches = (old.patches or [ ]) ++ [ ./opencode-packages/deferred-mcp.patch ];
})
