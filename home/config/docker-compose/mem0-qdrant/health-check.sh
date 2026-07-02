#!/usr/bin/env bash
# Mem0 Self-Hosted: Qdrant Health Check

set -euo pipefail

QDRANT_URL="${QDRANT_URL:-http://localhost:6333}"
QDRANT_HEALTH_ATTEMPTS="${QDRANT_HEALTH_ATTEMPTS:-1}"
QDRANT_HEALTH_SLEEP_SECONDS="${QDRANT_HEALTH_SLEEP_SECONDS:-1}"

case "$QDRANT_HEALTH_ATTEMPTS" in
'' | *[!0-9]*)
  echo "QDRANT_HEALTH_ATTEMPTS must be a positive integer" >&2
  exit 2
  ;;
esac

if [ "$QDRANT_HEALTH_ATTEMPTS" -lt 1 ]; then
  echo "QDRANT_HEALTH_ATTEMPTS must be at least 1" >&2
  exit 2
fi

tmp_err="$(mktemp)"
trap 'rm -f "$tmp_err"' EXIT

last_error="not checked"

for ((attempt = 1; attempt <= QDRANT_HEALTH_ATTEMPTS; attempt++)); do
  : >"$tmp_err"

  set +e
  response="$(curl -sS -o /dev/null -w "%{http_code}" "$QDRANT_URL/collections" 2>"$tmp_err")"
  curl_status="$?"
  set -e

  if [ "$curl_status" -eq 0 ] && [ "$response" = "200" ]; then
    echo "Qdrant is healthy at $QDRANT_URL"
    exit 0
  fi

  if [ "$curl_status" -ne 0 ]; then
    curl_error="$(tr '\n' ' ' <"$tmp_err" | sed 's/[[:space:]]*$//')"
    last_error="curl failed with exit $curl_status"
    if [ -n "$curl_error" ]; then
      last_error="$last_error: $curl_error"
    fi
  else
    last_error="HTTP $response"
  fi

  if [ "$attempt" -lt "$QDRANT_HEALTH_ATTEMPTS" ]; then
    echo "Qdrant not ready at $QDRANT_URL ($last_error); retrying..." >&2
    sleep "$QDRANT_HEALTH_SLEEP_SECONDS"
  fi
done

echo "Qdrant not responding at $QDRANT_URL ($last_error)" >&2
exit 1
