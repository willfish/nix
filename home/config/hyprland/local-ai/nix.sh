# shellcheck shell=bash
# NixOS host integration, sourced after upstream definitions and access.sh.
# Never run Omarchy's installer or an agent's imperative updater.
# Child shells expand their own arguments after the terminal opens.
# shellcheck disable=SC2016
cmd_setup() { die "Dependencies are managed by NixOS. Rebuild this host's configuration; no installer was run."; }
cmd_update() { die "Agents are managed by Nix. Update the dotfiles inputs and run hmswitch."; }
cmd_outdated() { :; }
docker_group_reexec() { :; }

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
  if [[ ${LOCAL_AI_NATIVE_STRATA:-0} == 1 ]] && ! nvidia_ready; then
    readiness_line unsupported "Activate hardware.nvidia-container-toolkit.enable on Andromeda for catalogue models. Native Strata is independent of Docker."
    return
  fi
  readiness_line ready
}

native_healthy() {
  [[ -s $LOCAL_AI_STRATA_KEY ]] || return 1
  curl --fail --silent --max-time 2 --output /dev/null \
    -H @<(printf 'Authorization: Bearer %s\n' "$(<"$LOCAL_AI_STRATA_KEY")") \
    http://127.0.0.1:8081/v1/models
}

native_snapshot() {
  local state prepared=false healthy=false
  state=$(systemctl --user show local-llm.service --property=ActiveState --value) || state=unknown
  [[ ! -f $LOCAL_AI_STRATA_READY ]] || prepared=true
  if [[ $state == active ]] && native_healthy; then healthy=true; fi
  jq -nc --arg state "$state" --argjson prepared "$prepared" --argjson healthy "$healthy" \
    '{state: $state, prepared: $prepared, healthy: $healthy}'
}

cmd_snapshot() {
  local native=null
  if [[ ${LOCAL_AI_NATIVE_STRATA:-0} == 1 ]]; then native=$(native_snapshot); fi
  upstream_snapshot | jq --argjson native "$native" '. + {nativeStrata: $native, updates: {}, updatesAt: ""}'
}

cmd_native() {
  [[ ${LOCAL_AI_NATIVE_STRATA:-0} == 1 ]] || die "Native Strata is configured only on Andromeda"
  case ${1:-} in
  start)
    [[ -f $LOCAL_AI_STRATA_READY ]] || die "Prepare Strata's weights first"
    if ! systemctl --user is-active --quiet local-llm.service; then
      local used
      used=$(nvidia-smi --id=0 --query-gpu=memory.used --format=csv,noheader,nounits) || die "Cannot check GPU memory"
      [[ $used =~ ^[0-9]+$ ]] || die "Cannot read GPU memory usage"
      ((used <= 2048)) || die "GPU already in use. Stop its current model explicitly before starting Strata."
    fi
    systemctl --user start --no-block local-llm.service || die "Could not start Strata"
    ;;
  stop) systemctl --user stop --no-block local-llm.service || die "Could not stop Strata" ;;
  prepare) omarchy-launch-tui --app-id=org.local-ai.strata-prepare bash -c 'strata-fetch; result=$?; read -r -p "Press Enter to close"; exit "$result"' ;;
  open)
    native_healthy || die "Strata is not answering yet. Check its service log."
    omarchy-launch-tui --app-id=org.local-ai.strata bash -c 'cd -- "$1" && exec pi --provider andromeda --model qwen3.8-flash-next' local-ai "$(get folder)"
    ;;
  log) omarchy-launch-tui --app-id=org.local-ai.strata-log journalctl --user -u local-llm.service -f ;;
  *) die "Unknown native Strata action" ;;
  esac
}
