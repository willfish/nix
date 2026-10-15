#!/usr/bin/env bash
# Pull every radio archive MP3 from the public Supabase bucket into DEST.
# Resumable: existing files of the exact expected size are skipped, partial
# files are continued with curl -C -. Re-run after an interruption.
set -uo pipefail

DEST="${1:-/srv/media/radio-archive/audio}"
URLS="${2:-./urls.txt}"
JOBS="${JOBS:-6}"
BASE="$(cd "$(dirname "$URLS")" && pwd)"
LOG="$BASE/fetch.log"
DONE="$BASE/done.txt"
FAIL="$BASE/failed.txt"
EXPECT="$BASE/expected_sizes.tsv"

mkdir -p "$DEST"
: >>"$LOG"
touch "$DONE" "$FAIL"

fetch_one() {
  url="$1"
  name="${url##*/}"
  out="$DEST/$name"
  want="$(awk -F'\t' -v n="$name" '$2==n {print $1; exit}' "$EXPECT")"
  if [ -n "${want:-}" ] && [ -f "$out" ] &&
    [ "$(stat -c%s "$out")" -eq "$want" ]; then
    return 0
  fi
  # Supabase answers a Range request whose start is 0 or past the end with 400,
  # so only a genuinely truncated download may be resumed.
  if [ -f "$out" ]; then
    have="$(stat -c%s "$out")"
    if [ "$have" -eq 0 ] || [ -z "${want:-}" ] || [ "$have" -ge "$want" ]; then
      rm -f "$out"
    fi
  fi
  for attempt in 1 2 3; do
    if curl -fsSL --connect-timeout 15 -m 1800 --retry 5 --retry-all-errors \
      --retry-delay 5 -C - -o "$out" "$url" 2>>"$LOG"; then
      got="$(stat -c%s "$out" 2>/dev/null || echo 0)"
      if [ -z "${want:-}" ] || [ "$got" -eq "$want" ]; then
        printf '%s\n' "$name" >>"$DONE"
        return 0
      fi
      # A finished body of the wrong length cannot be repaired by resuming.
      rm -f "$out"
    fi
    sleep $((attempt * 10))
  done
  printf '%s\t%s\n' "$name" "$url" >>"$FAIL"
  return 1
}

export -f fetch_one
export DEST LOG DONE FAIL EXPECT

total=$(grep -c . "$URLS")
echo "$(date -Is) start: $total urls, $JOBS workers -> $DEST" | tee -a "$LOG"
# shellcheck disable=SC2016
grep . "$URLS" | xargs -P "$JOBS" -n 1 -I{} bash -c 'fetch_one "$@"' _ {}
rc=$?
ok=$(wc -l <"$DONE")
bad=$(awk 'END{print NR}' "$FAIL")
echo "$(date -Is) finished rc=$rc ok=$ok failed=$bad of $total" | tee -a "$LOG"
