#!/usr/bin/env bash
# One budget for every CLI/MCP invocation and its detached daemon children.
# Never fall back to an unbounded launch when the user manager is unavailable.
if ! grep -q '/agent-browser.slice/' /proc/self/cgroup; then
  memory_kib=$(awk '$1 == "MemTotal:" { print $2; exit }' /proc/meminfo)
  if [[ ! $memory_kib =~ ^[0-9]+$ ]]; then
    echo "agent-browser: cannot determine physical memory" >&2
    exit 1
  fi

  # 1/16 of RAM, between 512 MiB and 4 GiB. Throttle at 75% of that.
  memory_max=$((memory_kib * 1024 / 16))
  ((memory_max >= 536870912)) || memory_max=536870912
  ((memory_max <= 4294967296)) || memory_max=4294967296
  memory_high=$((memory_max * 3 / 4))

  systemctl --user set-property --runtime agent-browser.slice \
    "MemoryHigh=$memory_high" "MemoryMax=$memory_max" \
    MemorySwapMax=268435456

  exec systemd-run --user --scope --quiet --slice=agent-browser.slice \
    --property=OOMPolicy=kill --description="Bounded browser automation" @binary@ "$@"
fi
