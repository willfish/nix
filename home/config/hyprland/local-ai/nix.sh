# shellcheck shell=bash
# NixOS owns dependencies and agent updates; the plugin owns Docker deployments.
cmd_setup() { die "Dependencies are managed by NixOS. Rebuild this host's configuration; no installer was run."; }
cmd_update() { die "Agents are managed by Nix. Update the dotfiles inputs and run hmswitch."; }
cmd_outdated() { :; }
docker_group_reexec() { :; }

STRATA_ID=qwen3.8-flash-next.strata.128k.rtx-5090-32gb
STRATA_IMAGE_ID=""
if [[ ${LOCAL_AI_STRATA:-0} == 1 ]]; then
  STRATA_IMAGE_ID=$(<"$LOCAL_AI_STRATA_IMAGE_ID")
  [[ $STRATA_IMAGE_ID =~ ^sha256:[0-9a-f]{64}$ ]] || die "Invalid Nix Strata image identity"
  # Keep refreshed upstream recipes, but never let a registry replace the local recipe.
  mkdir -p "$STATE/catalogues"
  merged="$STATE/catalogues/$(sha256sum "$RECIPES" "$PANEL/strata-recipe.json" | sha256sum | cut -c1-64).json"
  if [[ ! -f $merged ]]; then
    jq --slurpfile local "$PANEL/strata-recipe.json" --arg id "$STRATA_ID" '
      .hardware["rtx-5090-32gb"].recipes |= ([$local[0]] + map(select(.id != $id)))' "$RECIPES" >"$merged.$$"
    mv -f "$merged.$$" "$merged"
  fi
  RECIPES=$merged
  # All gateways on this host use the existing key. It never enters the image or argv.
  KEY=$LOCAL_AI_STRATA_KEY
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
  if [[ ${LOCAL_AI_STRATA:-0} == 1 ]] && ! nvidia_ready; then
    readiness_line unsupported "Activate hardware.nvidia-container-toolkit.enable on Andromeda for GPU containers."
    return
  fi
  readiness_line ready
}

is_strata() { [[ ${LOCAL_AI_STRATA:-0} == 1 && ${1%--*} == "$STRATA_ID" ]]; }

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

pull() {
  if [[ -n $STRATA_IMAGE_ID && $1 == "$STRATA_IMAGE_ID" ]]; then
    docker load --input "$LOCAL_AI_STRATA_IMAGE" >/dev/null
    docker image inspect "$STRATA_IMAGE_ID" >/dev/null || die "Nix Strata image did not load"
  else
    upstream_pull "$@"
  fi
}

# The imported weights are not catalogue downloads and must never be deleted by Forget.
cmd_forget() {
  if is_strata "${1:-}"; then die "Strata reuses your existing weights; catalogue removal cannot delete them."; fi
  upstream_forget "$@"
}
cmd_download() {
  if is_strata "${1:-}"; then
    [[ ${2:-} != off ]] || return 0
    [[ -f $LOCAL_AI_STRATA_READY ]] || die "Prepare Strata's weights with strata-fetch first (large download)."
    pull "$STRATA_IMAGE_ID" engine
  else
    upstream_download "$@"
  fi
}
cmd_card() {
  if is_strata "${1:-}"; then
    printf '# Qwen3.8 Flash-Next with Strata\n\nReuses the existing UD-Q4_K_XL weights and MTP pack.\n128K context, RTX 5090, 80 GiB resident expert budget.\nPrepare missing weights with strata-fetch. Catalogue removal keeps these weights.\n'
  else
    upstream_card "$@"
  fi
}

strata_check() {
  is_strata "$1" || return 0
  [[ -s $KEY ]] || die "Existing local model API key missing"
  [[ -f $LOCAL_AI_STRATA_READY && -f $LOCAL_AI_STRATA_DATA/packs/unsloth-ud-q4_k_xl/native_experts.txt ]] ||
    die "Prepare Strata's weights with strata-fetch first"
  owned "$LOCAL_AI_STRATA_DATA"
}

# Called only for the locally pinned recipe, after upstream's policy and allocation checks.
strata_engine_options() {
  is_strata "$1" || return 0
  printf '%s\0' --volume "$LOCAL_AI_STRATA_DATA:/models/strata:ro" \
    --user "$(id -u):$(id -g)" --cap-drop ALL --read-only --tmpfs /tmp:rw,nosuid,nodev,size=256m
}
strata_gateway_options() {
  is_strata "$1" || return 0
  # Preserve the authenticated endpoint used by the Andromeda Pi provider.
  printf '%s\0' --publish 8081:12434
}

cmd_snapshot() {
  upstream_snapshot | jq --arg id "$STRATA_ID" --argjson prepared "$([[ ${LOCAL_AI_STRATA:-0} == 1 && -f ${LOCAL_AI_STRATA_READY:-/nonexistent} ]] && echo true || echo false)" '
    . + {updates: {}, updatesAt: ""} |
    (.kinds[].models[] | select(.id == $id)) |= (.downloaded = $prepared |
      if $prepared then . else .unfit = "prepare weights with strata-fetch first" end)'
}

# Suspend must release all catalogue GPU models, not just the former native service.
cmd_stop_all() {
  local id
  local deployments
  deployments=$(cmd_snapshot) || die "Cannot inspect running models; suspend cancelled"
  while IFS= read -r id; do
    cmd_stop "$id" || die "Could not stop $id; suspend cancelled"
  done < <(jq -r '.deployments[] | select(any(.keys[]; startswith("cpu:") | not)) | .id' <<<"$deployments")
}
