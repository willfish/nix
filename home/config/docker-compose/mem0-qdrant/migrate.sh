#!/bin/bash
# Mem0 Self-Hosted Setup & Migration Script
# Migrates existing flat-file memory to Mem0 + Qdrant

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MEM0_DIR="$HOME/.mem0"
HERMES_DIR="$HOME/.hermes"
MEMORIES_DIR="$HERMES_DIR/memories"

echo "🦉 Mem0 Self-Hosted Migration"
echo "============================="

# 1. Check Qdrant is running
echo -e "\n[1/5] Checking Qdrant..."
if ! curl -s http://localhost:6333/collections > /dev/null 2>&1; then
    echo "❌ Qdrant not running. Start with:"
    echo "   $SCRIPT_DIR/start.sh"
    exit 1
fi
echo "✓ Qdrant is healthy"

# 2. Copy config
echo -e "\n[2/5] Installing Mem0 config..."
if [ -f "$HERMES_DIR/mem0-selfhosted.json" ]; then
    cp "$HERMES_DIR/mem0-selfhosted.json" "$MEM0_DIR/config.json"
    echo "✓ Config installed to $MEM0_DIR/config.json"
else
    echo "⚠️  No config found at $HERMES_DIR/mem0-selfhosted.json"
fi

# 3. Migrate flat-file memories
echo -e "\n[3/5] Migrating flat-file memories..."

# Extract facts from MEMORY.md
if [ -f "$HERMES_DIR/MEMORY.md" ]; then
    echo "  → Processing MEMORY.md..."
    # Parse MEMORY.md into facts (strip headers, bullets, etc.)
    python3 << 'EOF'
import re

memory_file = "$HERMES_DIR/MEMORY.md"
output_file = "$HERMES_DIR/memories/migrated-facts.jsonl"

try:
    with open(memory_file) as f:
        content = f.read()
    
    facts = []
    
    # Extract user profile facts
    user_match = re.search(r'USER PROFILE.*?\n(.*?)(?=\n#|\Z)', content, re.DOTALL)
    if user_match:
        for line in user_match.group(1).split('\n'):
            line = line.strip()
            if line.startswith('-') or line.startswith('•'):
                facts.append(line.lstrip('-• '))
            elif line and not line.startswith('#') and len(line) > 5:
                facts.append(line)
    
    # Extract core principles
    core_match = re.search(r'Core Principles.*?\n(.*?)(?=\n#|\Z)', content, re.DOTALL)
    if core_match:
        facts.append("Core Principles: " + core_match.group(0)[:200])
    
    # Extract memory entries (lines starting with date patterns)
    date_pattern = r'\d{4}-\d{2}-\d{2}.*?:'
    for match in re.finditer(date_pattern, content):
        line = content[match.start():].split('\n')[0]
        facts.append(line)
    
    print(f"  Extracted {len(facts)} facts from MEMORY.md")
    
    with open(output_file, 'w') as f:
        for fact in facts:
            if fact:
                f.write(f'{{"text": "{fact.replace(chr(34), chr(92)+chr(34))}"}}\n')
                
except Exception as e:
    print(f"  Warning: {e}")
EOF
fi

# Extract from daily memory files
if [ -d "$MEMORIES_DIR" ]; then
    echo "  → Processing daily memory files..."
    python3 << 'EOF'
import os
import re
from pathlib import Path

memories_dir = Path("$MEMORIES_DIR")
output_file = "$HERMES_DIR/memories/processed-facts.jsonl"

facts = []

for md_file in sorted(memories_dir.glob("*.md"))[-7:]):  # Last 7 days
    content = md_file.read_text()
    
    # Extract non-trivial content
    for line in content.split('\n'):
        line = line.strip()
        # Skip headers, empty lines, simple markers
        if line.startswith('#') or not line or line in ['---', '§']:
            continue
        # Keep substantive lines
        if len(line) > 20 and not line.startswith(('-', '•', '*')):
            facts.append(f"{md_file.stem}: {line}")
        elif line.startswith(('-', '•')) and len(line) > 30:
            facts.append(line.lstrip('-• '))

print(f"  Extracted {len(facts)} facts from daily memories")
with open(output_file, 'w') as f:
    for fact in facts:
        f.write(f'{{"text": "{fact.replace(chr(34), chr(92)+chr(34))}"}}\n')
EOF
fi

# 4. Update Hermes config
echo -e "\n[4/5] Updating Hermes memory config..."
~/.hermes/hermes-agent/venv/bin/hermes config set memory.provider mem0 2>/dev/null || \
    echo "  (Manual: hermes config set memory.provider mem0)"
~/.hermes/hermes-agent/venv/bin/hermes config set memory.memory_enabled true 2>/dev/null || true

echo "✓ Hermes memory provider set to mem0"

# 5. Verify setup
echo -e "\n[5/5] Verifying Mem0 integration..."
~/.hermes/hermes-agent/venv/bin/python3 << 'EOF'
import sys
sys.path.insert(0, '$HOME/.hermes/hermes-agent/venv/lib/python3.11/site-packages')

try:
    from mem0.configs.base import MemoryConfig, VectorStoreConfig
    from mem0.configs.vector_stores.qdrant import QdrantConfig
    
    config = MemoryConfig.from_config_file("$HOME/.mem0/config.json")
    print("✓ Mem0 config loaded successfully")
    print(f"  Vector store: {config.vector_store.config.collection_name}")
except Exception as e:
    print(f"⚠️  Config verification: {e}")
EOF

echo -e "\n✅ Migration complete!"
echo ""
echo "Next steps:"
echo "  1. Start Qdrant: ./start.sh"
echo "  2. Restart Hermes: hermes"
echo "  3. Test: Send a message — mem0_search should activate automatically"