# shellcheck shell=bash
# Region recording for Hyprland. Super+V starts after a slurp selection and
# stops on the next press. gpu-screen-recorder does the capture; this script
# only picks the region, tracks the process, and opens the saved file.

runtime_dir="${XDG_RUNTIME_DIR:?}/hypr-record"
mkdir -p "$runtime_dir"
chmod 700 "$runtime_dir"
pid_file="$runtime_dir/pid"
path_file="$runtime_dir/path"
log_file="$runtime_dir/log"
lock_dir="$runtime_dir/lock"

recording() {
  [[ -f $pid_file ]] || return 1
  local pid
  pid=$(<"$pid_file")
  [[ $pid =~ ^[0-9]+$ ]] || return 1
  kill -0 "$pid" 2>/dev/null
}

refresh_bar() {
  pkill -RTMIN+8 waybar >/dev/null 2>&1 || true
}

output_dir() {
  local dirs="${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs"
  if [[ -f $dirs ]]; then
    # shellcheck disable=SC1090
    source "$dirs"
  fi
  printf '%s\n' "${XDG_VIDEOS_DIR:-$HOME/Videos}"
}

even_size() {
  local n=$1
  if ((n < 2)); then
    return 1
  fi
  printf '%s\n' $((n - n % 2))
}

fail() {
  local message=$1
  rm -f "$pid_file"
  refresh_bar
  notify-send -u critical -t 8000 "Screen recording" "$message"
  return 1
}

launch() {
  gpu-screen-recorder "$@" >"$log_file" 2>&1 &
  printf '%s\n' "$!"
}

wait_started() {
  local pid=$1 path=$2 i=0
  while ((i < 25)); do
    if [[ -f $path ]]; then
      if kill -0 "$pid" 2>/dev/null; then
        return 0
      fi
      wait "$pid" || true
      return 1
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" || true
      return 1
    fi
    sleep 0.2
    i=$((i + 1))
  done
  kill -0 "$pid" 2>/dev/null
}

stop_recording() {
  local pid path trimmed
  pid=$(<"$pid_file")
  path=$(<"$path_file")
  kill -INT "$pid" 2>/dev/null || true
  local i=0
  while kill -0 "$pid" 2>/dev/null && ((i < 50)); do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    notify-send -u critical -t 5000 "Screen recording" "The recorder had to be force-stopped. The video may be incomplete."
  else
    wait "$pid" 2>/dev/null || true
  fi
  rm -f "$pid_file"
  refresh_bar
  if [[ ! -s $path ]]; then
    fail "No video was saved."
    return
  fi
  trimmed="${path%.mp4}-trim.mp4"
  if ffmpeg -y -ss 0.1 -i "$path" -c copy "$trimmed" -loglevel error; then
    mv "$trimmed" "$path"
  else
    rm -f "$trimmed"
  fi
  wl-copy -- "$path"
  notify-send -t 8000 "Screen recording saved" "Path copied. Opening the file."
  nohup nautilus --select "$path" >/dev/null 2>&1 &
}

start_recording() {
  local geom w h x y region dir path pid
  geom=$(slurp -f '%w %h %x %y') || return 0
  read -r w h x y <<<"$geom"
  w=$(even_size "$w") || {
    fail "The selection is too small."
    return
  }
  h=$(even_size "$h") || {
    fail "The selection is too small."
    return
  }
  region="${w}x${h}+${x}+${y}"
  dir=$(output_dir)
  mkdir -p "$dir"
  path="$dir/screenrecording-$(date +%Y-%m-%d_%H-%M-%S).mp4"
  if [[ ! -x /run/wrappers/bin/gsr-kms-server ]]; then
    fail "GPU capture is not enabled yet. Switch the NixOS system so gsr-kms-server is available."
    return
  fi
  pid=$(launch -w "$region" -f 60 -fm cfr -k auto -fallback-cpu-encoding yes -a default_output -ac aac -o "$path")
  if ! wait_started "$pid" "$path"; then
    rm -f "$path"
    pid=$(launch -w "$region" -f 60 -fm cfr -k auto -fallback-cpu-encoding yes -o "$path")
    if ! wait_started "$pid" "$path"; then
      wait "$pid" 2>/dev/null || true
      fail "Recording did not start. $(tail -n 6 "$log_file")"
      return
    fi
  fi
  printf '%s\n' "$pid" >"$pid_file"
  printf '%s\n' "$path" >"$path_file"
  refresh_bar
  notify-send -t 2500 "Recording" "Super+V stops and opens the file."
}

with_lock() {
  if ! mkdir "$lock_dir" 2>/dev/null; then
    notify-send -t 2000 "Screen recording" "Already busy."
    return 1
  fi
  trap 'rmdir "$lock_dir" 2>/dev/null || true' RETURN
  "$@"
}

case ${1:-} in
status)
  recording
  ;;
*)
  if recording; then
    with_lock stop_recording
  else
    with_lock start_recording
  fi
  ;;
esac
