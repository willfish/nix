#!/usr/bin/env bats

setup_file() {
  export FLAKE_ROOT="$BATS_TEST_DIRNAME/.."
}

theme_value() {
  local configuration="$1"
  local key="$2"

  # shellcheck disable=SC2016
  FLAKE_ROOT="$FLAKE_ROOT" HERDR_CONFIGURATION="$configuration" HERDR_THEME_KEY="$key" \
    nix eval --impure --raw --expr '
      let
        flake = builtins.getFlake (builtins.getEnv "FLAKE_ROOT");
        configuration = builtins.getEnv "HERDR_CONFIGURATION";
        key = builtins.getEnv "HERDR_THEME_KEY";
        source = flake.homeConfigurations.${configuration}.config.home.file.".config/herdr/config.toml".source;
        config =
          if builtins.isPath source || builtins.isString source then
            builtins.fromTOML (builtins.readFile source)
          else
            builtins.fromJSON source.value;
      in
        config.theme.${key}
    '
}

assert_theme() {
  local configuration="$1"
  local expected_dark="$2"
  local expected_light="$3"

  [ "$(theme_value "$configuration" dark_name)" = "$expected_dark" ]
  [ "$(theme_value "$configuration" light_name)" = "$expected_light" ]
}

@test "generates a distinct Herdr theme pair for each host" {
  assert_theme "william@andromeda" rose-pine rose-pine-dawn
  assert_theme "william@foundation" tokyo-night tokyo-night-day
  assert_theme "william@starfish" solarized solarized-light
  assert_theme "william@terminus" catppuccin catppuccin-latte
  assert_theme william-darwin gruvbox gruvbox-light
}
