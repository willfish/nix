#!/usr/bin/env bash
set -euo pipefail

index=${RADIO_ARCHIVE_INDEX:-http://terminus:8085/playlists/index.m3u}
dest=${RADIO_ARCHIVE_PLAYLISTS:-${CLIAMP_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/cliamp}/playlists}
mark='# radio-archive-generated'

mkdir -p "$dest"
work=$(mktemp -d "$dest/.radio-archive.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

if ! curl --fail --silent --show-error --max-time 8 "$index" -o "$work/index.m3u"; then
  echo 'radio archive index unavailable; keeping existing playlists'
  exit 0
fi
# Do not prune cached stations on an empty or unexpected response.
if ! grep -q '^#EXTM3U' "$work/index.m3u"; then
  echo 'radio archive index invalid; keeping existing playlists' >&2
  exit 1
fi

declare -A seen=()
title=''
count=0
failed=0
while IFS= read -r line || [[ -n $line ]]; do
  line=${line%$'\r'}
  case "$line" in
  '#EXTINF:'*) title=${line#*,} ;;
  http://* | https://* | file://*)
    [[ -n $title ]] || continue
    name=${title//\//-}
    name=${name//\\/-}
    title=''
    [[ $name != . && $name != .. ]] || continue
    path="$dest/$name.toml"
    seen["$name.toml"]=1
    if [[ -e $path || -L $path ]] && ! grep -qxF "$mark" "$path"; then
      continue
    fi
    if ! curl --fail --silent --show-error --max-time 30 "$line" -o "$work/station.m3u"; then
      failed=$((failed + 1))
      continue
    fi
    # TOML track URLs are not expanded by cliamp. Import the actual songs,
    # in an isolated config so a failed refresh cannot replace a cached list.
    rm -f -- "$work/config/playlists/station.toml"
    if ! CLIAMP_CONFIG_DIR="$work/config" cliamp playlist import --name station "$work/station.m3u" >/dev/null; then
      failed=$((failed + 1))
      continue
    fi
    if ! grep -q '^\[\[track\]\]' "$work/config/playlists/station.toml"; then
      echo "radio archive station empty: $name; keeping existing playlist" >&2
      failed=$((failed + 1))
      continue
    fi
    {
      echo "$mark"
      cat "$work/config/playlists/station.toml"
    } >"$work/playlist.toml"
    mv -f -- "$work/playlist.toml" "$path"
    count=$((count + 1))
    ;;
  esac
done <"$work/index.m3u"

if ((${#seen[@]} > 0)); then
  for path in "$dest"/*.toml; do
    [[ -e $path ]] || continue
    if grep -qxF "$mark" "$path" && [[ ! -v seen[${path##*/}] ]]; then
      rm -- "$path"
    fi
  done
  old="$dest/radio-archive.toml"
  if [[ -e $old ]] && grep -qF 'playlists/index.m3u' "$old"; then
    rm -- "$old"
  fi
fi
echo "wrote $count archive playlists ($failed unavailable)"
