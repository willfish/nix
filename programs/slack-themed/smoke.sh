#!/usr/bin/env bash
set -euo pipefail
package=${1:?Usage: smoke.sh PACKAGE [OUTPUT_DIRECTORY]}
output=${2:-$(mktemp -d /tmp/slack-theme-smoke.XXXXXX)}
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output"
output=$(realpath "$output")
mkdir "$output/runtime" "$output/harness"
# Keep Electron's shipped libraries and resources. Only the disposable test
# runtime's executable and ASAR differ, never the installed package or profile.
cp -as "$package/lib/slack/." "$output/runtime/"
find "$output/runtime" -type d -exec chmod u+w {} +
rm "$output/runtime/slack"
cp --reflink=auto "$package/lib/slack/slack" "$output/runtime/slack"
asar extract "$package/lib/slack/resources/app.asar" "$output/extracted"
cp "$output/extracted/dist/dotfiles-theme.cjs" "$output/harness/"
cp "$source_dir/smoke.cjs" "$output/harness/"
printf '{"name":"slack-theme-smoke","version":"1.0.0","main":"smoke.cjs"}\n' >"$output/harness/package.json"
rm -r "$output/runtime/resources"
mkdir "$output/runtime/resources"
asar pack "$output/harness" "$output/runtime/resources/app.asar"
export SLACK_THEME_SMOKE_SOURCE="${XDG_STATE_HOME:-$HOME/.local/state}/theme-menu/active/hyprland.conf"
export SLACK_THEME_SMOKE_OUTPUT="$output"
export XDG_STATE_HOME="$output/state"
export XDG_CONFIG_HOME="$output/config"
unset SLACK_DOTFILES_THEME
"$output/runtime/slack" --ozone-platform=wayland
printf 'Native-renderer screenshots: %s/active.png and %s/light.png\n' "$output" "$output"
