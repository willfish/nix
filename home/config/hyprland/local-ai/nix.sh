# shellcheck shell=bash
# NixOS owns dependencies and agent updates; the plugin owns Docker deployments.
cmd_setup() { die "Dependencies are managed by NixOS. Rebuild this host's configuration; no installer was run."; }
cmd_update() { die "Agents are managed by Nix. Update the dotfiles inputs and run hmswitch."; }
cmd_outdated() { :; }
docker_group_reexec() { :; }

# Keep existing Pi/OpenCode credentials usable without putting a key in the store or argv.
if [[ -n ${LOCAL_AI_KEY_FILE:-} ]]; then
  # shellcheck disable=SC2034
  KEY=$LOCAL_AI_KEY_FILE
fi

readiness() {
  command -v docker >/dev/null || {
    readiness_line unsupported "Enable virtualisation.docker in NixOS"
    return
  }
  docker_reachable || {
    readiness_line docker-down "Docker socket unavailable. Enable Docker and docker group membership in NixOS; log in again after group changes."
    return
  }
  docker info >/dev/null 2>&1 || {
    readiness_line docker-down "Docker is not answering"
    return
  }
  if [[ ${LOCAL_AI_NVIDIA:-0} == 1 ]] && ! nvidia_ready; then
    readiness_line unsupported "Activate hardware.nvidia-container-toolkit.enable on Andromeda for GPU containers."
    return
  fi
  readiness_line ready
}

nvidia_cdi_device() {
  local uuid=$1 index=$2 devices
  devices=$(docker info --format '{{json .DiscoveredDevices}}') || die "Cannot inspect NVIDIA CDI devices"
  if jq -e --arg id "nvidia.com/gpu=$uuid" 'any(.[]; .ID == $id)' <<<"$devices" >/dev/null; then
    printf 'nvidia.com/gpu=%s' "$uuid"
  elif [[ $index == 0 && $(nvidia-smi --query-gpu=uuid --format=csv,noheader) == "$uuid" ]] &&
    jq -e 'any(.[]; .ID == "nvidia.com/gpu=0")' <<<"$devices" >/dev/null; then
    # NixOS defaults to indexed CDI names. A single GPU makes index zero unambiguous.
    printf 'nvidia.com/gpu=0'
  else
    die "Enable UUID device names in NixOS NVIDIA container toolkit configuration"
  fi
}

# Only the primary deployment owns the stable client endpoint, never a second instance.
# Docker's published ports bypass the host firewall, so this reaches the LAN too.
# The gateway still requires the API key.
shared_gateway_bindings() {
  [[ -n ${LOCAL_AI_SHARED_RECIPE:-} && $1 == "$LOCAL_AI_SHARED_RECIPE" ]] || return 0
  printf '%s\n' '0.0.0.0:8081:12434'
}

cmd_snapshot() {
  upstream_snapshot | jq '. + {updates: {}, updatesAt: ""}'
}

# Labels catch deployments missing from the catalogue state, including CPU models.
local_ai_containers() {
  docker ps --quiet --filter "label=$LABEL=1"
}

# Stop the former model service before containers so Restart= cannot reload it.
# Keep catalogue cleanup for starting workers, then catch remaining containers.
cmd_stop_all() {
  local id deployments ids load_state containers
  load_state=$(systemctl --user show local-llm.service --property=LoadState --value) ||
    die "Cannot inspect the local model service; sleep cancelled"
  if [[ $load_state != not-found ]]; then
    systemctl --user stop local-llm.service || die "Could not stop the local model service; sleep cancelled"
  fi
  deployments=$(cmd_snapshot) || die "Cannot inspect running models; sleep cancelled"
  ids=$(jq -r '.deployments[] | select(any(.keys[]; startswith("cpu:") | not)) | .id' <<<"$deployments") ||
    die "Cannot read GPU model allocations; sleep cancelled"
  while IFS= read -r id; do
    [[ -n $id ]] || continue
    cmd_stop "$id" || die "Could not stop $id; sleep cancelled"
  done <<<"$ids"
  containers=$(local_ai_containers | sort -u) || die "Cannot inspect Local AI containers; sleep cancelled"
  while IFS= read -r id; do
    [[ -n $id ]] || continue
    docker stop --timeout 30 "$id" >/dev/null || die "Could not stop Local AI container $id; sleep cancelled"
  done <<<"$containers"
  containers=$(local_ai_containers) || die "Cannot verify stopped Local AI containers; sleep cancelled"
  [[ -z $containers ]] || die "Local AI containers are still running; sleep cancelled"
}
