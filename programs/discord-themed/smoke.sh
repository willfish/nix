#!/usr/bin/env bash
set -euo pipefail
package=${1:?Usage: smoke.sh PACKAGE [OUTPUT_DIRECTORY]}
output=${2:-$(mktemp -d /tmp/discord-theme-smoke.XXXXXX)}
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output"
output=$(realpath "$output")
mkdir "$output/runtime" "$output/harness"
# Change only a disposable runtime. Preserve the packaged libraries and wrapper.
cp -as "$package/opt/Discord/." "$output/runtime/"
find "$output/runtime" -type d -exec chmod u+w {} +
rm "$output/runtime/.Discord-wrapped" "$output/runtime/Discord"
cp --reflink=auto "$package/opt/Discord/.Discord-wrapped" "$output/runtime/.Discord-wrapped"
node - "$package/opt/Discord" "$output/runtime" <<'JS'
const fs = require("node:fs"), path = require("node:path");
const [original, runtime] = process.argv.slice(2);
fs.writeFileSync(path.join(runtime, "Discord"), fs.readFileSync(path.join(original, "Discord"), "utf8").replaceAll(original, runtime), { mode: 0o755 });
JS
asar extract "$package/opt/Discord/resources/app.asar" "$output/extracted"
cp "$output/extracted/dotfiles-theme.cjs" "$output/harness/"
cp "$source_dir/smoke.cjs" "$output/harness/"
# Exercise the packaged startup line, replacing only Discord's bootstrap payload.
node - "$output/extracted/bundle.js" "$output/harness/bundle.js" <<'JS'
const fs = require("node:fs"), assert = require("node:assert/strict");
const [source, target] = process.argv.slice(2);
const hook = fs.readFileSync(source, "utf8").split("\n")[0];
assert.equal(hook, 'require("./dotfiles-theme.cjs").install();');
fs.writeFileSync(target, hook + '\nrequire("./smoke.cjs");\n');
JS
printf '{"name":"discord-theme-smoke","version":"1.0.0","main":"bundle.js"}\n' >"$output/harness/package.json"
rm -r "$output/runtime/resources"
mkdir "$output/runtime/resources"
asar pack "$output/harness" "$output/runtime/resources/app.asar"
export DISCORD_THEME_SMOKE_SOURCE="${XDG_STATE_HOME:-$HOME/.local/state}/theme-menu/active/hyprland.conf"
export DISCORD_THEME_SMOKE_OUTPUT="$output"
export XDG_STATE_HOME="$output/state"
export XDG_CONFIG_HOME="$output/config"
export XDG_CACHE_HOME="$output/cache"
export DISCORD_USER_DATA_DIR="$output/config"
"$output/runtime/Discord" --ozone-platform=wayland
if [[ ${DISCORD_DOTFILES_THEME:-1} != 0 ]]; then
  printf 'Native-renderer screenshots: %s/active.png and %s/light.png\n' "$output" "$output"
fi
