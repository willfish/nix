#!/bin/bash
# Start Qdrant for Mem0 semantic memory

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_DIR="$(dirname "$SCRIPT_DIR")"

cd "$COMPOSE_DIR" || exit 1

if command -v docker-compose &> /dev/null; then
    docker-compose up -d
elif command -v docker &> /dev/null && docker compose version &> /dev/null; then
    docker compose up -d
else
    echo "Error: Docker or Docker Compose not found"
    exit 1
fi

echo "Qdrant started. API available at http://localhost:6333"