{
  pkgs,
  testMode ? false,
}:
pkgs.runCommandCC "newrelic-brief"
  {
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [
      pkgs.glib
      pkgs.json_c
    ];
    meta = {
      description = "Read-only Pi New Relic briefs with a five-day desktop notebook";
      mainProgram = "newrelic-brief";
      platforms = pkgs.lib.platforms.linux;
    };
  }
  ''
    mkdir -p "$out/bin"
    $CC -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 ${pkgs.lib.optionalString testMode "-DBRIEF_TEST"} \
      -DBRIEF_ZONEINFO='"${pkgs.tzdata}/share/zoneinfo/Europe/London"' \
      $(pkg-config --cflags gio-2.0 json-c) \
      -o "$out/bin/newrelic-brief" ${./main.c} \
      $(pkg-config --libs gio-2.0 json-c)
  ''
