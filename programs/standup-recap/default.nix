{ pkgs }:
pkgs.runCommandCC "standup-recap"
  {
    meta = {
      description = "Fill today's daily note with the previous working day's activity via Pi";
      mainProgram = "standup-recap";
      platforms = pkgs.lib.platforms.unix;
    };
  }
  ''
    mkdir -p "$out/bin"
    $CC -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 \
      -o "$out/bin/standup-recap" \
      ${./main.c}
  ''
