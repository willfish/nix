#!/usr/bin/env bash
# Render the two main Mermaid diagrams from the README for visual inspection.
# Usage: ./render-readme-diagrams.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
OUT_DIR="$SCRIPT_DIR/rendered"

mkdir -p "$OUT_DIR"

echo "Rendering Architecture diagram..."
if command -v mmdc >/dev/null 2>&1; then
  mmdc -i "$SRC_DIR/architecture.mmd" -o "$OUT_DIR/architecture.png" --scale 2 --backgroundColor "#ffffff"
else
  npx @mermaid-js/mermaid-cli -i "$SRC_DIR/architecture.mmd" -o "$OUT_DIR/architecture.png" --scale 2 --backgroundColor "#ffffff"
fi

echo "Rendering Repository Structure diagram..."
if command -v mmdc >/dev/null 2>&1; then
  mmdc -i "$SRC_DIR/repo-structure.mmd" -o "$OUT_DIR/repo-structure.png" --scale 2 --backgroundColor "#ffffff"
else
  npx @mermaid-js/mermaid-cli -i "$SRC_DIR/repo-structure.mmd" -o "$OUT_DIR/repo-structure.png" --scale 2 --backgroundColor "#ffffff"
fi

echo ""
echo "Done! PNGs are in: $OUT_DIR"
echo "Open them and apply the Diagram Review Checklist."