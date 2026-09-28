# shellcheck shell=bash
# Region recording for Hyprland. Super+V starts after a slurp selection and
# stops on the next press. Super+Alt+V adds a camera square. Ctrl+Super+V
# records only the camera and microphone. gpu-screen-recorder does the
# capture; this script picks the frame, tracks the process, and opens the file.

runtime_dir="${XDG_RUNTIME_DIR:?}/hypr-record"
mkdir -p "$runtime_dir"
chmod 700 "$runtime_dir"
pid_file="$runtime_dir/pid"
path_file="$runtime_dir/path"
log_file="$runtime_dir/log"
lock_dir="$runtime_dir/lock"
webcam_pid_file="$runtime_dir/webcam"
mode_file="$runtime_dir/mode"
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

# Centered 16:9 frame inside a monitor: x, y, width, height.
# Ctrl+Super+V records this frame, not the desktop around it.
message_frame() {
  local mw=$1 mh=$2 margin=80 maxw=1280 maxh=720
  local avail_w avail_h w h x y
  avail_w=$((mw - 2 * margin))
  avail_h=$((mh - 2 * margin))
  if ((avail_w < 320 || avail_h < 180)); then
    return 1
  fi
  w=$maxw
  h=$maxh
  if ((w > avail_w)); then
    w=$avail_w
  fi
  if ((h > avail_h)); then
    h=$avail_h
  fi
  if ((w * 9 > h * 16)); then
    w=$((h * 16 / 9))
  else
    h=$((w * 9 / 16))
  fi
  w=$(even_size "$w") || return 1
  h=$(even_size "$h") || return 1
  x=$(((mw - w) / 2))
  y=$(((mh - h) / 2))
  printf '%s %s %s %s\n' "$x" "$y" "$w" "$h"
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
  local device=$1 format=${2:-} crop=${3:-square}
  local -a format_args=() vf_args=()
  if [[ -n $format ]]; then
    format_args=(--demuxer-lavf-o="$format")
  fi
  if [[ $crop == square ]]; then
    vf_args=(--vf='lavfi=[crop=min(iw\,ih):min(iw\,ih)]')
  fi
  mpv "av://v4l2:${device}" \
    --profile=low-latency --untimed --no-cache \
    "${format_args[@]}" \
    "${vf_args[@]}" \
    --title=WebcamOverlay --force-media-title=WebcamOverlay \
    --wayland-app-id=WebcamOverlay \
    --no-border --no-audio --no-osc --osd-level=0 \
    --really-quiet \
    >/dev/null 2>&1 &
  printf '%s\n' "$!" >"$webcam_pid_file"
}

place_overlay() {
  local x=$1 y=$2 w=$3 h=${4:-$3} i=0 address
  while ((i < 40)); do
    address=$(hyprctl clients -j | jq -r 'first(.[] | select(.title == "WebcamOverlay" or .class == "WebcamOverlay") | .address) // empty')
    if [[ -n $address ]]; then
      hyprctl dispatch setfloating "address:${address}" >/dev/null || true
      hyprctl dispatch resizewindowpixel "exact ${w} ${h},address:${address}" >/dev/null
      hyprctl dispatch movewindowpixel "exact ${x} ${y},address:${address}" >/dev/null
      hyprctl dispatch alterzorder top "address:${address}" >/dev/null || true
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

record_label() {
  if [[ -f $mode_file && $(<"$mode_file") == message ]]; then
    printf '%s\n' "Video message"
    return
  fi
  printf '%s\n' "Screen recording"
}

fail() {
  local message=$1 title
  title=$(record_label)
  rm -f "$pid_file" "$mode_file"
  refresh_bar
  notify-send -u critical -t 8000 "$title" "$message"
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
  local title
  title=$(record_label)
  rm -f "$pid_file"
  refresh_bar
  if [[ ! -s $path ]]; then
    fail "No video was saved."
    return
  fi
  rm -f "$mode_file"
  trimmed="${path%.mp4}-trim.mp4"
  if ffmpeg -y -ss 0.1 -i "$path" -c copy "$trimmed" -loglevel error; then
    mv "$trimmed" "$path"
  else
    rm -f "$trimmed"
  fi
  wl-copy -- "$path"
  notify-send -t 8000 "$title saved" "Path copied. Opening the file."
  nohup nautilus --select "$path" >/dev/null 2>&1 &
}

start_capture() {
  local region=$1 path=$2
  shift 2
  launch -w "$region" -f 60 -fm cfr -k auto -fallback-cpu-encoding yes "$@" -o "$path"
}

focused_monitor() {
  hyprctl monitors -j | jq -er 'first(.[] | select(.focused)) | "\(.x) \(.y) \(.width) \(.height)"'
}

# Camera and microphone only. The preview window is the whole frame, so the
# saved file does not include the desktop around it.
start_message() {
  local device mx my mw mh x y w h region dir path pid
  printf '%s\n' message >"$mode_file"
  if [[ ! -x $kms_server ]]; then
    fail "GPU capture is not enabled yet. Switch the NixOS system so gsr-kms-server is available."
    return
  fi
  device=$(camera_device) || {
    fail "No camera was found."
    return
  }
  read -r mx my mw mh < <(focused_monitor) || {
    fail "No monitor was found."
    return
  }
  read -r x y w h < <(message_frame "$mw" "$mh") || {
    fail "The camera window does not fit on this monitor."
    return
  }
  x=$((x + mx))
  y=$((y + my))
  stop_webcam
  open_webcam "$device" "video_size=1280x720,input_format=mjpeg,framerate=30" frame
  if ! place_overlay "$x" "$y" "$w" "$h"; then
    stop_webcam
    fail "The camera did not open."
    return
  fi
  sleep 0.4
  read -r x y w h < <(hyprctl clients -j | jq -er 'first(.[] | select(.title == "WebcamOverlay" or .class == "WebcamOverlay") | "\(.at[0]) \(.at[1]) \(.size[0]) \(.size[1])")') || {
    stop_webcam
    fail "The camera did not open."
    return
  }
  w=$(even_size "$w") || {
    stop_webcam
    fail "The camera window is too small."
    return
  }
  h=$(even_size "$h") || {
    stop_webcam
    fail "The camera window is too small."
    return
  }
  dir=$(output_dir)
  mkdir -p "$dir"
  path="$dir/videomessage-$(date +%Y-%m-%d_%H-%M-%S).mp4"
  region="${w}x${h}+${x}+${y}"
  pid=$(launch -w "$region" -f 30 -fm cfr -k auto -fallback-cpu-encoding yes -cursor no -a default_input -ac aac -o "$path")
  if ! wait_started "$pid" "$path"; then
    rm -f "$path"
    pid=$(launch -w "$region" -f 30 -fm cfr -k auto -fallback-cpu-encoding yes -cursor no -o "$path")
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
  notify-send -t 2500 "Video message" "Press it again to stop and open the file."
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
message-box)
  shift
  message_frame "$@"
  ;;
status)
  recording
  ;;
*)
  if recording; then
    with_lock stop_recording
  elif [[ ${1:-} == face ]]; then
    with_lock start_recording face
  elif [[ ${1:-} == camera ]]; then
    with_lock start_message
  else
    with_lock start_recording
  fi
  ;;
esac
