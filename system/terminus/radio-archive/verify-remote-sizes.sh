#!/usr/bin/env bash
# Confirm every pulled MP3 matches the byte length the bucket reports today.
#
# The catalog's own metadata claims a wrong size for a dozen rows, and a rerun
# of the downloader can only detect truncation against a trusted number, so this
# asks storage directly: one 1-byte range request per object, whose
# Content-Range trailer carries the real total. Ranges keep the transfer
# negligible. Keep the
# worker count modest; the bucket is shared with the live player.
#
#   bash /srv/media/radio-archive/scripts/verify-remote-sizes.sh [jobs]
set -uo pipefail
BASE=${BASE:-/srv/media/radio-archive}
BUCKET=${BUCKET:-https://xewajlyswijmjxuajhif.supabase.co/storage/v1/object/public/audio-files}
JOBS=${1:-8}
cd "$BASE" || exit

check_one() {
  name="$1"
  local_size="$(stat -c%s "audio/$name" 2>/dev/null || echo missing)"
  total="$(curl -fsS -o /dev/null -m 60 --retry 3 --retry-delay 5 -r 0-0 \
    -w '%header{content-range}' "$BUCKET/$name" 2>/dev/null | awk -F/ '{print $2}')"
  if [[ $local_size != "$total" || -z $total ]]; then
    printf '%s\tlocal=%s\tremote=%s\n' "$name" "$local_size" "${total:-none}"
  fi
}
export -f check_one
export BASE BUCKET

cd "$BASE" || exit
: >work/remote_size_mismatch.txt
find audio -maxdepth 1 -name '*.mp3' -printf '%f\n' | sort >work/audio_names.txt
# shellcheck disable=SC2016
xargs -P "$JOBS" -n 1 -I{} bash -c 'check_one "$@"' _ {} <work/audio_names.txt \
  >>work/remote_size_mismatch.txt
echo "$(date -Is) checked $(wc -l <work/audio_names.txt) objects, mismatches $(wc -l <work/remote_size_mismatch.txt)"
