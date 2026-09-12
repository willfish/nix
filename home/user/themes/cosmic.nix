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
    # The importer can log write failures without a nonzero status. Require both
    # complete outputs, plus their builders, before publishing any configuration.
    for mode in Light Dark; do
      for field in background primary secondary accent is_dark list_button; do
        test -s "$out/cosmic/com.system76.CosmicTheme.$mode/v1/$field"
      done
      test -s "$out/cosmic/com.system76.CosmicTheme.$mode.Builder/v1/palette"
    done
    grep -qx true "$out/cosmic/com.system76.CosmicTheme.Dark/v1/is_dark"
    grep -qx false "$out/cosmic/com.system76.CosmicTheme.Light/v1/is_dark"
  ''
