#!/usr/bin/env bash
set -euo pipefail

script=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/sync-radio-archive-playlists.sh
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export RADIO_ARCHIVE_INDEX="file://$work/index.m3u"
export RADIO_ARCHIVE_PLAYLISTS="$work/config/playlists"
export CLIAMP_CONFIG_DIR="$work/config"
mkdir -p "$RADIO_ARCHIVE_PLAYLISTS"

cat >"$work/station.m3u" <<'EOF'
#EXTM3U
#EXTINF:258,Jeno Jando - Prelude and Fugue
http://terminus:8085/audio/81293.mp3
#EXTINF:297,Janos Sebestyen - Fantasia and Fugue
http://terminus:8085/audio/49453.mp3
EOF
printf '#EXTM3U\n#EXTINF:-1,05 The Duke\nfile://%s/station.m3u\n' "$work" >"$work/index.m3u"
printf '# radio-archive-generated\n[[track]]\npath = "nested.m3u"\n' >"$RADIO_ARCHIVE_PLAYLISTS/05 The Duke.toml"
printf '# radio-archive-generated\n' >"$RADIO_ARCHIVE_PLAYLISTS/stale.toml"
printf '# personal playlist\n' >"$RADIO_ARCHIVE_PLAYLISTS/personal.toml"

bash "$script"
playlist="$RADIO_ARCHIVE_PLAYLISTS/05 The Duke.toml"
[[ $(grep -c '^\[\[track\]\]' "$playlist") == 2 ]]
grep -qxF 'path = "http://terminus:8085/audio/81293.mp3"' "$playlist"
grep -qxF 'duration_secs = 258' "$playlist"
if grep -q '\.m3u' "$playlist"; then
  echo 'FAIL: playlist contains a nested M3U instead of songs' >&2
  exit 1
fi
cliamp playlist show '05 The Duke' | grep -q '2 tracks'
[[ ! -e $RADIO_ARCHIVE_PLAYLISTS/stale.toml ]]
grep -qxF '# personal playlist' "$RADIO_ARCHIVE_PLAYLISTS/personal.toml"

cp "$playlist" "$work/expected.toml"
rm "$work/station.m3u"
bash "$script"
cmp "$playlist" "$work/expected.toml"
rm "$work/index.m3u"
bash "$script"
cmp "$playlist" "$work/expected.toml"
echo 'PASS: song import, cliamp loading, ownership, and offline cache retention'
