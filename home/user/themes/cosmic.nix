{ pkgs, theme }:
pkgs.runCommand "host-cosmic-themes"
  {
    nativeBuildInputs = [ pkgs.python3 ];
    palette = pkgs.writeText "host-palette.json" (builtins.toJSON theme);
  }
  ''
    # Never import themes into the builder's desktop. The native CLI writes only
    # into this sandbox, deriving all component/hover/disabled colours itself.
    export HOME="$TMPDIR/home"
    export XDG_CONFIG_HOME="$out"
    export XDG_CACHE_HOME="$TMPDIR/cache"
    export XDG_DATA_HOME="$TMPDIR/data"
    export XDG_STATE_HOME="$TMPDIR/state"
    export XDG_RUNTIME_DIR="$TMPDIR/runtime"
    unset HOST_XDG_CONFIG_HOME DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY
    mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$out"
    python ${./cosmic.py} "$palette" "$out/builders"
    for mode in light dark; do
      ${pkgs.cosmic-settings}/bin/cosmic-settings appearance import "$out/builders/$mode.ron"
    done
    # The importer can log empty-config parse noise without a nonzero status.
    # COSMIC 1.2 wrote schema v1; 1.6 writes v2. Require the version the CLI
    # actually produced, not a hardcoded path.
    theme_ver=
    builder_ver=
    for candidate in v2 v1; do
      if [ -z "$theme_ver" ] && [ -s "$out/cosmic/com.system76.CosmicTheme.Light/$candidate/background" ]; then
        theme_ver=$candidate
      fi
      if [ -z "$builder_ver" ] && [ -s "$out/cosmic/com.system76.CosmicTheme.Light.Builder/$candidate/palette" ]; then
        builder_ver=$candidate
      fi
    done
    test -n "$theme_ver"
    test -n "$builder_ver"
    for mode in Light Dark; do
      for field in background primary secondary accent is_dark list_button; do
        test -s "$out/cosmic/com.system76.CosmicTheme.$mode/$theme_ver/$field"
      done
      test -s "$out/cosmic/com.system76.CosmicTheme.$mode.Builder/$builder_ver/palette"
    done
    grep -qx true "$out/cosmic/com.system76.CosmicTheme.Dark/$theme_ver/is_dark"
    grep -qx false "$out/cosmic/com.system76.CosmicTheme.Light/$theme_ver/is_dark"
  ''
