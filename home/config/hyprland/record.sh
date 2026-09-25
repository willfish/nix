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
webcam_pid_file="$runtime_dir/webcam"
kms_server="${HYPR_RECORD_KMS_SERVER:-/run/wrappers/bin/gsr-kms-server}"

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

# Bottom-right square inside the selected region: x, y, side.
overlay_box() {
  local w=$1 h=$2 x=$3 y=$4 margin=24 side max ox oy
  if ((w < h)); then
    side=$w
  else
    side=$h
  fi
  side=$((side / 4))
  if ((side > 320)); then
    side=320
  fi
  if ((side < 160)); then
    side=160
  fi
  if ((w < h)); then
    max=$w
  else
    max=$h
  fi
  max=$((max - 2 * margin))
  if ((max < 64)); then
    return 1
  fi
  if ((side > max)); then
    side=$max
  fi
  side=$((side - side % 2))
  ox=$((x + w - side - margin))
  oy=$((y + h - side - margin))
  if ((ox < x + margin)); then
    ox=$((x + margin))
  fi
  if ((oy < y + margin)); then
    oy=$((y + margin))
  fi
  printf '%s %s %s\n' "$ox" "$oy" "$side"
}

camera_device() {
  local dev index
  for dev in /dev/video*; do
    [[ -e $dev ]] || return 1
    index=$(cat "/sys/class/video4linux/${dev##*/}/index" 2>/dev/null || echo 1)
    if [[ $index == 0 ]]; then
      printf '%s\n' "$dev"
      return 0
    fi
  done
  return 1
}

stop_webcam() {
  local pid
  if [[ -f $webcam_pid_file ]]; then
    pid=$(<"$webcam_pid_file")
    if [[ $pid =~ ^[0-9]+$ ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
    rm -f "$webcam_pid_file"
  fi
}

open_webcam() {
  local device=$1 format=${2:-}
  local -a format_args=()
  if [[ -n $format ]]; then
    format_args=(--demuxer-lavf-o="$format")
  fi
  mpv "av://v4l2:${device}" \
    --profile=low-latency --untimed --no-cache \
    "${format_args[@]}" \
    --vf='lavfi=[crop=min(iw\,ih):min(iw\,ih)]' \
    --title=WebcamOverlay --force-media-title=WebcamOverlay \
    --wayland-app-id=WebcamOverlay \
    --no-border --no-audio --no-osc --osd-level=0 \
    --really-quiet \
    >/dev/null 2>&1 &
  printf '%s\n' "$!" >"$webcam_pid_file"
}

place_overlay() {
  local x=$1 y=$2 size=$3 i=0 address
  while ((i < 40)); do
    address=$(hyprctl clients -j | jq -r 'first(.[] | select(.title == "WebcamOverlay" or .class == "WebcamOverlay") | .address) // empty')
    if [[ -n $address ]]; then
      hyprctl dispatch setfloating "address:${address}" >/dev/null || true
      hyprctl dispatch resizewindowpixel "exact ${size} ${size},address:${address}" >/dev/null
      hyprctl dispatch movewindowpixel "exact ${x} ${y},address:${address}" >/dev/null
      return 0
    fi
    sleep 0.05
    i=$((i + 1))
  done
  return 1
}

start_webcam() {
  local x=$1 y=$2 size=$3 device
  device=$(camera_device) || {
    fail "No camera was found."
    return
  }
  stop_webcam
  open_webcam "$device" "video_size=1280x720,input_format=mjpeg,framerate=30"
  if ! place_overlay "$x" "$y" "$size"; then
    stop_webcam
    open_webcam "$device"
    if ! place_overlay "$x" "$y" "$size"; then
      stop_webcam
      fail "The camera did not open."
      return
    fi
  fi
  # Let the overlay settle so the recording does not catch it sliding in.
  sleep 0.4
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
  stop_webcam
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

start_capture() {
  local region=$1 path=$2
  shift 2
  launch -w "$region" -f 60 -fm cfr -k auto -fallback-cpu-encoding yes "$@" -o "$path"
}

start_recording() {
  local mode=${1:-screen} geom w h x y region dir path pid overlay=()
  if [[ ! -x $kms_server ]]; then
    fail "GPU capture is not enabled yet. Switch the NixOS system so gsr-kms-server is available."
    return
  fi
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
  if [[ $mode == face ]]; then
    read -r overlay < <(overlay_box "$w" "$h" "$x" "$y") || {
      fail "The selection is too small for the camera."
      return
    }
    # shellcheck disable=SC2086
    start_webcam $overlay || return
  fi
  dir=$(output_dir)
  mkdir -p "$dir"
  path="$dir/screenrecording-$(date +%Y-%m-%d_%H-%M-%S).mp4"
  if [[ $mode == face ]]; then
    pid=$(start_capture "$region" "$path" -a "default_output|default_input" -ac aac)
    if ! wait_started "$pid" "$path"; then
      rm -f "$path"
      pid=$(start_capture "$region" "$path" -a default_input -ac aac)
    fi
  else
    pid=$(start_capture "$region" "$path" -a default_output -ac aac)
  fi
  if ! wait_started "$pid" "$path"; then
    rm -f "$path"
    pid=$(start_capture "$region" "$path")
    if ! wait_started "$pid" "$path"; then
      wait "$pid" 2>/dev/null || true
      stop_webcam
      fail "Recording did not start. $(tail -n 6 "$log_file")"
      return
    fi
  fi
  printf '%s\n' "$pid" >"$pid_file"
  printf '%s\n' "$path" >"$path_file"
  refresh_bar
  notify-send -t 2500 "Recording" "Press it again to stop and open the file."
}

release_lock() {
  rmdir "$lock_dir" 2>/dev/null || true
}

# A previous run can die under errexit before its RETURN trap fires. An empty
# lock with no other recorder is that leftover, not a live selection.
lock_is_stale() {
  local pid
  while read -r pid; do
    [[ -z $pid || $pid -eq $$ ]] && continue
    return 1
  done < <(pgrep -x hypr-record || true)
  return 0
}

with_lock() {
  if ! mkdir "$lock_dir" 2>/dev/null; then
    if lock_is_stale && release_lock && mkdir "$lock_dir" 2>/dev/null; then
      :
    else
      notify-send -t 2000 "Screen recording" "Already busy."
      return 1
    fi
  fi
  # errexit leaves the shell without returning from this function, so RETURN
  # is too late. EXIT still runs.
  trap release_lock EXIT
  "$@"
  release_lock
}

case ${1:-} in
box)
  shift
  overlay_box "$@"
  ;;
status)
  recording
  ;;
*)
  if recording; then
    with_lock stop_recording
  elif [[ ${1:-} == face ]]; then
    with_lock start_recording face
  else
    with_lock start_recording
  fi
  ;;
esac
