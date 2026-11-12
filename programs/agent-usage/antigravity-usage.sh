#!/usr/bin/env bash
set -euo pipefail

# Keep user plugins, hooks, MCP servers and repositories out of a quota probe.
# Local authentication stays with agy's native system keyring, never a token copy.
probe=$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/omarchy-antigravity-usage.XXXXXXXX")
trap 'rm -rf -- "$probe"' EXIT
mkdir -p "$probe/home" "$probe/bin" "$probe/work"
for launcher in open xdg-open; do
  printf '#!/bin/sh\nexit 1\n' >"$probe/bin/$launcher"
  chmod 700 "$probe/bin/$launcher"
done

# A transient unit contains and reaps all CLI children, including detached ones.
# No model prompt is sent: /usage is the CLI's built-in quota command.
systemd-run --user --quiet --pipe --wait --collect \
  --working-directory="$probe/work" \
  --property=RuntimeMaxSec=35s --property=TimeoutStopSec=1s \
  --property=KillMode=control-group --property=UMask=0077 \
  --property=StandardError=null \
  env -i \
  HOME="$probe/home" PATH="$probe/bin:$PATH" BROWSER="$probe/bin/open" \
  XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
  DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-}" \
  SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}" \
  AGY_CLI_DISABLE_AUTO_UPDATE=true \
  agy -p /usage --output-format json --print-timeout 30s \
  --settings '{"toolPermission":"strict","agentMode":"plan","useG1Credits":false}'
