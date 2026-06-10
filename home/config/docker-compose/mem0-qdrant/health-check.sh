#!/bin/bash
# Mem0 Self-Hosted: Qdrant Health Check

QDRANT_URL="${QDRANT_URL:-http://localhost:6333}"

response=$(curl -s -o /dev/null -w "%{http_code}" "$QDRANT_URL/collections" 2>/dev/null)

if [ "$response" = "200" ]; then
  echo "✓ Qdrant is healthy at $QDRANT_URL"
  exit 0
else
  echo "✗ Qdrant not responding at $QDRANT_URL (HTTP $response)"
  exit 1
fi
