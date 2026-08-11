set shell := ["bash", "-euo", "pipefail", "-c"]

# Show capacity and basic health for every reachable host, or one named host.
health host="":
    #!/usr/bin/env bash
    selected='{{ host }}'
    if [ -n "$selected" ]; then
      hosts=("$selected")
    else
      hosts=(andromeda foundation relay starfish terminus)
    fi

    found=0
    local_host="$(hostname -s | tr '[:upper:]' '[:lower:]')"
    for host_name in "${hosts[@]}"; do
      target=""
      if [ "${host_name,,}" = "$local_host" ]; then
        target="local"
      else
        for candidate in "$host_name.local" "$host_name"; do
          if nc -z -w 1 "$candidate" 22 </dev/null >/dev/null 2>&1; then
            target="$candidate"
            break
          fi
        done
        if [ -z "$target" ]; then
          continue
        fi

        ssh -o BatchMode=yes -o ConnectTimeout=3 -o HostKeyAlias="$host_name" \
          "$target" true </dev/null >/dev/null 2>&1 &
        probe_pid=$!
        (sleep 5; kill "$probe_pid" 2>/dev/null || true) &
        timer_pid=$!
        if wait "$probe_pid"; then
          probe_status=0
        else
          probe_status=$?
        fi
        kill "$timer_pid" 2>/dev/null || true
        wait "$timer_pid" 2>/dev/null || true
        if [ "$probe_status" -ne 0 ]; then
          continue
        fi
      fi

      found=1
      printf '\n== %s ==\n' "$host_name"
      if [ "$target" = "local" ]; then
        runner=(bash -s)
      else
        runner=(
          ssh -o BatchMode=yes -o ConnectTimeout=3 -o HostKeyAlias="$host_name"
          "$target" bash -s
        )
      fi
      "${runner[@]}" <<'REMOTE'
        uptime
        if command -v duf >/dev/null 2>&1; then
          duf --only local --output mountpoint,size,used,avail,usage
        else
          df -h
        fi
        if command -v systemctl >/dev/null 2>&1; then
          failed="$(systemctl --failed --no-legend --plain 2>/dev/null || true)"
          if [ -n "$failed" ]; then
            printf "Failed units:\n%s\n" "$failed"
          else
            echo "Failed units: none"
          fi
        else
          echo "Failed units: not applicable (no systemd)"
        fi
        if command -v zpool >/dev/null 2>&1; then
          zpool status -x
        fi
        if command -v systemctl >/dev/null 2>&1; then
          scrub_timers="$(systemctl list-timers --all --no-legend --no-pager 2>/dev/null | grep 'zfs-scrub' || true)"
          if [ -n "$scrub_timers" ]; then
            printf "ZFS scrub timers:\n%s\n" "$scrub_timers"
          fi
        fi
        if systemctl cat smartd.service >/dev/null 2>&1; then
          printf "smartd: %s\n" "$(systemctl is-active smartd.service)"
        fi
    REMOTE
    done

    if [ "$found" -eq 0 ]; then
      echo "No requested hosts are reachable over LAN or Tailscale." >&2
      exit 69
    fi

# Run detailed Terminus ZFS and physical-disk checks (prompts for remote sudo).
terminus-health:
    #!/usr/bin/env bash
    target="terminus"
    if nc -z -w 1 terminus.local 22 </dev/null >/dev/null 2>&1; then
      target="terminus.local"
    fi
    ssh -t -o ConnectTimeout=5 -o HostKeyAlias=terminus "$target" '
      duf --only local --output mountpoint,size,used,avail,usage
      systemctl list-timers --all --no-pager zfs-scrub.timer
      systemctl is-active smartd.service
      sudo zpool status tank
      echo "== /dev/sda =="; sudo smartctl -H /dev/sda
      echo "== /dev/sdb =="; sudo smartctl -H /dev/sdb
      echo "== /dev/nvme0 =="; sudo smartctl -H /dev/nvme0
    '
