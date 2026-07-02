#!/usr/bin/env bash
# Start Qdrant for Mem0 semantic memory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_DIR="$SCRIPT_DIR"

compose() {
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    docker compose "$@"
  elif command -v docker-compose >/dev/null 2>&1 && docker-compose version >/dev/null 2>&1; then
    docker-compose "$@"
  else
    echo "Error: Docker Compose not found. Install docker with the compose plugin or docker-compose." >&2
    return 127
  fi
}

cd "$COMPOSE_DIR" || exit 1

compose up -d

QDRANT_HEALTH_ATTEMPTS="${QDRANT_HEALTH_ATTEMPTS:-30}" \
  QDRANT_HEALTH_SLEEP_SECONDS="${QDRANT_HEALTH_SLEEP_SECONDS:-1}" \
  "$SCRIPT_DIR/health-check.sh"

echo "Qdrant started. API available at http://localhost:6333"
