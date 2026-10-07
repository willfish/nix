{ pkgs }:
pkgs.runCommandCC "herdr-sleep-inhibit"
  {
    meta = {
      description = "Inhibit system sleep while Herdr agents are working";
      mainProgram = "herdr-agent-awake";
      platforms = pkgs.lib.platforms.linux;
    };
  }
  ''
    mkdir -p "$out/bin"
    $CC -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 \
      -DSYSTEMD_INHIBIT=\"${pkgs.lib.getExe' pkgs.systemd "systemd-inhibit"}\" \
      -DSLEEP_BIN=\"${pkgs.lib.getExe' pkgs.coreutils "sleep"}\" \
      -o "$out/bin/herdr-agent-awake" \
      ${./main.c}
  ''
