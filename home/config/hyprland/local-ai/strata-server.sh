#!/bin/sh
set -eu

# Local AI mounts the verified preparation read-only. Keep server logs, chat
# settings and conversation snapshots in this container's writable runtime.
mkdir -p /tmp/strata
sed 's|/prepared/install/|/tmp/strata/|g' \
  /prepared/install/strata-unsloth-ud-iq4_xs-v31.json >/tmp/strata/config.json

# Only the authenticated Local AI gateway can reach this internal Docker
# network. It owns the API key and the public model alias.
cd /opt/strata/terminal
exec python/bin/python3 serve/server.py --engine strata \
  --config /tmp/strata/config.json --host 0.0.0.0 --port 8080 --api-key none
