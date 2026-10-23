#!/usr/bin/env bash
# Offline regression check. Never call the real power or local-model commands.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/bin"
export SESSION_TEST_LOG="$scratch/log"
export SESSION_TEST_STOPPED="$scratch/stopped"
: >"$SESSION_TEST_STOPPED"
export HYPR_SESSION_MENU_FILE="$scratch/menu.tsv"
export HYPR_SESSION_FUZZEL_CONFIG=/dev/null
export HYPR_SESSION_STOP_LOCAL_LLM=1
printf 'Suspend\tsuspend\t0\tsuspend\nHibernate\thibernate\t0\thibernate\n' >"$HYPR_SESSION_MENU_FILE"
cat >"$scratch/bin/command-stub" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
name=${0##*/}
case "$name" in
  notify-send) exit 0 ;;
  fuzzel) input=$(</dev/stdin); grep -Fx -- "$SESSION_TEST_CHOICE" <<<"$input" ;;
  omarchy-local-ai)
    printf 'cleanup %s\n' "$*" >>"$SESSION_TEST_LOG"
    exit "${SESSION_TEST_CLEANUP_STATUS:-0}"
    ;;
  systemctl)
    if [[ $1 == --user ]]; then
      case "$2" in
        show) printf '%s\n' "${SESSION_TEST_UNIT_LOAD:-not-found}"; exit "${SESSION_TEST_UNIT_QUERY_STATUS:-0}" ;;
        stop) printf 'service %s\n' "$3" >>"$SESSION_TEST_LOG"; exit "${SESSION_TEST_UNIT_STOP_STATUS:-0}" ;;
        *) exit 99 ;;
      esac
    else
      printf 'power %s\n' "$*" >>"$SESSION_TEST_LOG"
    fi
    ;;
  docker)
    case "$1" in
      ps)
        [[ ${SESSION_TEST_DOCKER_QUERY_STATUS:-0} == 0 ]] || exit 1
        case "$4" in
          label=io.omarchy.local-ai=1) ids=${SESSION_TEST_LABELLED:-} ;;
          ancestor=sha256:fixture) ids=${SESSION_TEST_STRATA:-} ;;
          *) exit 99 ;;
        esac
        while IFS= read -r id; do
          [[ -n $id ]] || continue
          if [[ ${SESSION_TEST_KEEP_RUNNING:-0} == 1 ]] || ! grep -Fxq -- "$id" "$SESSION_TEST_STOPPED"; then
            printf '%s\n' "$id"
          fi
        done <<<"$ids"
        ;;
      stop)
        [[ $2 == --timeout && $3 == 30 ]]
        printf 'container %s\n' "$4" >>"$SESSION_TEST_LOG"
        [[ ${SESSION_TEST_DOCKER_STOP_STATUS:-0} == 0 ]] || exit 1
        printf '%s\n' "$4" >>"$SESSION_TEST_STOPPED"
        ;;
      *) exit 99 ;;
    esac
    ;;
  *) exit 99 ;;
esac
STUB
chmod +x "$scratch/bin/command-stub"
for command in notify-send fuzzel omarchy-local-ai systemctl docker; do
  ln -s command-stub "$scratch/bin/$command"
done
export PATH="$scratch/bin:$PATH"
for action in suspend hibernate; do
  for route in direct select menu; do
    args=("$action")
    case "$route" in
    select) args=(select "$action") ;;
    menu)
      args=(menu)
      export SESSION_TEST_CHOICE="${action^}"
      ;;
    esac
    : >"$SESSION_TEST_LOG"
    bash -euo pipefail "$root/home/config/hyprland/session.sh" "${args[@]}"
    printf 'cleanup stop_all\npower %s --ignore-inhibitors\n' "$action" >"$scratch/expected"
    diff -u "$scratch/expected" "$SESSION_TEST_LOG"

    : >"$SESSION_TEST_LOG"
    if SESSION_TEST_CLEANUP_STATUS=1 bash -euo pipefail "$root/home/config/hyprland/session.sh" "${args[@]}" 2>"$scratch/error"; then
      echo "$route $action should cancel when cleanup fails" >&2
      exit 1
    fi
    grep -q "$action cancelled" "$scratch/error"
    printf 'cleanup stop_all\n' >"$scratch/expected"
    diff -u "$scratch/expected" "$SESSION_TEST_LOG"
  done
  : >"$SESSION_TEST_LOG"
  HYPR_SESSION_STOP_LOCAL_LLM=0 bash -euo pipefail "$root/home/config/hyprland/session.sh" "$action"
  printf 'power %s --ignore-inhibitors\n' "$action" >"$scratch/expected"
  diff -u "$scratch/expected" "$SESSION_TEST_LOG"
done

# Exercise the real GPU selection helper without Docker or model operations.
export LOCAL_AI_STRATA=0 LABEL=io.omarchy.local-ai
# shellcheck source=../home/config/hyprland/local-ai/nix.sh disable=SC1091
source "$root/home/config/hyprland/local-ai/nix.sh"
die() {
  printf '%s\n' "$*" >&2
  exit 1
}
cmd_snapshot() {
  printf '%s\n' "$SESSION_TEST_SNAPSHOT"
  return "${SESSION_TEST_SNAPSHOT_STATUS:-0}"
}
cmd_stop() {
  printf 'stop %s\n' "$1" >>"$SESSION_TEST_LOG"
  return "${SESSION_TEST_STOP_STATUS:-0}"
}
export SESSION_TEST_SNAPSHOT='{"deployments":[{"id":"qwen3.8-flash-next.strata.128k.rtx-5090-32gb","keys":["nvidia:GPU-fixture"]},{"id":"cpu-model","keys":["cpu:0"]},{"id":"other-gpu-model","keys":["amd:fixture"]}]}'
: >"$SESSION_TEST_LOG"
(cmd_stop_all)
printf 'stop qwen3.8-flash-next.strata.128k.rtx-5090-32gb\nstop other-gpu-model\n' >"$scratch/expected"
diff -u "$scratch/expected" "$SESSION_TEST_LOG"
for failure in snapshot parse stop; do
  (
    case "$failure" in
    snapshot) SESSION_TEST_SNAPSHOT_STATUS=1 ;;
    parse) SESSION_TEST_SNAPSHOT='invalid JSON' ;;
    stop) SESSION_TEST_STOP_STATUS=1 ;;
    esac
    cmd_stop_all
  ) 2>"$scratch/error" && {
    echo "$failure should cancel sleep" >&2
    exit 1
  }
  grep -q 'sleep cancelled' "$scratch/error"
done
: >"$SESSION_TEST_LOG"
SESSION_TEST_SNAPSHOT='{"deployments":[]}' cmd_stop_all
[[ ! -s $SESSION_TEST_LOG ]]
# Service shutdown must precede catalogue cleanup and residual Docker stops.
# The same Strata container appears under both filters but must stop only once.
export SESSION_TEST_UNIT_LOAD=loaded
export SESSION_TEST_LABELLED=$'labelled-cpu\nstrata-container'
export SESSION_TEST_STRATA=strata-container
export STRATA_IMAGE_ID=sha256:fixture
: >"$SESSION_TEST_LOG"
(cmd_stop_all)
printf 'service local-llm.service\nstop qwen3.8-flash-next.strata.128k.rtx-5090-32gb\nstop other-gpu-model\ncontainer labelled-cpu\ncontainer strata-container\n' >"$scratch/expected"
diff -u "$scratch/expected" "$SESSION_TEST_LOG"

# Direct Strata runs need no catalogue entry or Local AI label.
: >"$SESSION_TEST_LOG"
: >"$SESSION_TEST_STOPPED"
SESSION_TEST_UNIT_LOAD=not-found SESSION_TEST_LABELLED='' SESSION_TEST_SNAPSHOT='{"deployments":[]}' cmd_stop_all
printf 'container strata-container\n' >"$scratch/expected"
diff -u "$scratch/expected" "$SESSION_TEST_LOG"
for failure in service-query service-stop docker-query docker-stop survivor; do
  : >"$SESSION_TEST_STOPPED"
  (
    case "$failure" in
    service-query) export SESSION_TEST_UNIT_QUERY_STATUS=1 ;;
    service-stop) export SESSION_TEST_UNIT_STOP_STATUS=1 ;;
    docker-query) export SESSION_TEST_DOCKER_QUERY_STATUS=1 ;;
    docker-stop) export SESSION_TEST_DOCKER_STOP_STATUS=1 ;;
    survivor) export SESSION_TEST_KEEP_RUNNING=1 ;;
    esac
    cmd_stop_all
  ) 2>"$scratch/error" && {
    echo "$failure should cancel sleep" >&2
    exit 1
  }
  grep -q 'sleep cancelled' "$scratch/error"
done
printf 'PASS: sleep routes, service-first cleanup, tracked GPU models, labelled containers, direct Strata runs and failure cancellation\n'
