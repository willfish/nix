# Terminal Demo GIFs

When working on CLI tools, proactively generate animated GIF demos for READMEs and PR descriptions.

## Choose your approach

| Approach | Use when | Limitations |
|----------|----------|-------------|
| **asciinema + agg** | Simple CLI output | Cannot capture tmux, vim, htop, or other TUI apps |
| **gpu-screen-recorder + ffmpeg** | tmux, vim, htop, or any visual terminal content | Requires portal interaction, larger files |

## Approach 1: asciinema (simple CLI demos)

Best for demos that only show command output without full-screen TUI apps.

```bash
nix-shell -p asciinema asciinema-agg
```

**1. Write a driver script that simulates typing:**

```bash
#!/usr/bin/env bash
type_cmd() {
  local cmd="$1"
  for (( i=0; i<${#cmd}; i++ )); do
    printf '%s' "${cmd:$i:1}"
    sleep 0.04
  done
  sleep 0.3
  printf '\n'
}

pause() { sleep "${1:-1.5}"; }

clear
pause 0.5

type_cmd "mytool --version"
mytool --version
pause 1

type_cmd "mytool run example"
mytool run example
pause 2
```

**2. Record and convert:**

```bash
asciinema rec demo.cast --command 'bash demo-script.sh' --cols 80 --rows 24 --overwrite
agg demo.cast demo.gif --font-size 14 --speed 1
```

## Approach 2: gpu-screen-recorder (tmux, vim, htop, etc.)

Use this when the demo needs to show tmux panes, vim, htop, or other TUI applications that asciinema cannot capture.

**Why not wf-recorder?** It requires wlroots protocols (Sway, Hyprland). For COSMIC, GNOME, KDE, use gpu-screen-recorder which works via xdg-desktop-portal.

**1. Create a recording script (`demo/record-demo.sh`):**

```bash
#!/usr/bin/env bash
cd "$(dirname "$0")/.."

VIDEO_FILE="/tmp/demo.mp4"
GIF_FILE="demo.gif"

# Cleanup
tmux kill-session -t demo 2>/dev/null || true
rm -f "$VIDEO_FILE"

echo "Starting recording..."
read -p "Press Enter to begin, then select your terminal in the portal dialog..."

# Start recording (suppress output)
gpu-screen-recorder -w portal -f 24 -o "$VIDEO_FILE" >/dev/null 2>&1 &
RECORDER_PID=$!

read -p "Complete the screen share dialog, then press Enter to start the demo..."

if ! kill -0 $RECORDER_PID 2>/dev/null; then
    echo "Recording failed to start"
    exit 1
fi

clear

# === Your demo commands here ===
type_cmd() {
    local cmd="$1"
    for (( i=0; i<${#cmd}; i++ )); do
        printf '%s' "${cmd:$i:1}"
        sleep 0.035
    done
    sleep 0.25
    printf '\n'
}

type_cmd "cat demo/demo.yml"
cat demo/demo.yml
sleep 1.5

clear

# For tmux demos: auto-detach after delay
(sleep 4; tmux detach-client -s demo 2>/dev/null) &

type_cmd "mux start -p demo/demo.yml"
./build/mux start -p demo/demo.yml

sleep 0.5
# === End demo commands ===

# Stop recording
kill -INT $RECORDER_PID 2>/dev/null
sleep 2
wait $RECORDER_PID 2>/dev/null || true

# Cleanup
tmux kill-session -t demo 2>/dev/null || true

# Convert to GIF
ffmpeg -y -i "$VIDEO_FILE" \
    -vf "fps=12,scale=800:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer" \
    -loop 0 "$GIF_FILE"

rm -f "$VIDEO_FILE"
echo "Saved to $GIF_FILE"
```

**2. Add a Makefile target:**

```makefile
# Record demo GIF (uses xdg-desktop-portal for screen capture)
demo: build
	nix-shell -p gpu-screen-recorder ffmpeg --run "bash demo/record-demo.sh"
```

**3. Run with `make demo`**

## Tips

- Keep demos under 20 seconds — show the core workflow, not every flag
- Use `--cols 80 --rows 24` for asciinema for consistent GitHub README width
- The typing effect matters — a wall of text appearing instantly is hard to follow
- For video approach: select just the terminal content, not window decorations
- Commit the GIF to the repo (typically 100-500KB)
