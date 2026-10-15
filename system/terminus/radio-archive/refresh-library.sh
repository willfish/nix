#!/usr/bin/env bash
# Rebuild the radio archive library: verify the pull, expand it into the tagged
# tree, write tags, then regenerate the playlists players consume.
#
#   bash /etc/radio-archive/refresh-library.sh [--fetch] [--skip-tag]
#
# --fetch runs the resumable downloader first. id3v2 comes from nix so the NAS
# carries no permanent tagging package.
set -euo pipefail
BASE=${BASE:-/srv/media/radio-archive}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export RADIO_ARCHIVE_BASE="$BASE"
cd "$BASE"

do_fetch=0
skip_tag=0
for arg in "$@"; do
  case "$arg" in
  --fetch) do_fetch=1 ;;
  --skip-tag) skip_tag=1 ;;
  *)
    echo "unknown option: $arg" >&2
    exit 64
    ;;
  esac
done

if [[ $do_fetch -eq 1 ]]; then
  echo "== pulling missing audio =="
  bash "$SCRIPT_DIR/fetch-audio.sh" "$BASE/audio" "$BASE/work/urls.txt"
fi

echo "== verifying pulled audio =="
python3 "$SCRIPT_DIR/verify-audio.py" "$BASE" ||
  echo "warning: see work/bad_audio.txt, rerun with --fetch"

echo "== building the library tree =="
python3 "$SCRIPT_DIR/build-library.py" --apply

if [[ $skip_tag -eq 0 ]]; then
  echo "== writing ID3 tags =="
  nix shell --impure nixpkgs#id3v2 -c \
    python3 "$SCRIPT_DIR/tag-library.py" --apply
fi

echo "== generating station playlists =="
python3 "$SCRIPT_DIR/build-stations.py"

echo "== summary =="
printf 'audio    %s files, %s\n' "$(find audio -maxdepth 1 -type f | wc -l)" \
  "$(du -sh audio | cut -f1)"
printf 'library  %s files, %s\n' \
  "$(find music unsorted -type f 2>/dev/null | wc -l)" \
  "$(du -shc music unsorted 2>/dev/null | tail -1 | cut -f1)"
printf 'stations %s playlists\n' "$(find playlists -name '*.m3u' | wc -l)"
