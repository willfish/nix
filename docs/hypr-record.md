# Region screen recording with a camera overlay (Hyprland)

`hypr-record` is one Bash script plus three Hyprland keybinds. It records a
region you drag out with the mouse, using GPU encoding through
[gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder/about/), and
optionally floats a webcam square in the corner of that region so the camera
ends up inside the video. Pressing the same chord again stops the recording,
trims the first 0.1s, copies the file path to the clipboard and opens the file
manager on it.

Everything is in the single `hypr-record` script in this bundle; there is no
daemon and no config file. State lives in `$XDG_RUNTIME_DIR/hypr-record/` (pid,
output path, mode, log, lock).

## Chords

| Chord | Mode | Captures |
| --- | --- | --- |
| `Super+V` | screen | Dragged region, 60fps, system audio |
| `Super+Alt+V` | face | Dragged region plus a webcam square bottom-right, 60fps, system audio + microphone |
| `Super+Ctrl+V` | message | Camera and microphone only: a centred 16:9 frame (max 1280x720), 30fps, cursor excluded |

Each mode writes to `$XDG_VIDEOS_DIR` (from `~/.config/user-dirs.dirs`,
defaulting to `~/Videos`) as `screenrecording-<timestamp>.mp4` or
`videomessage-<timestamp>.mp4`.

## Requirements

| Package | Used for |
| --- | --- |
| `gpu-screen-recorder` | Capture and hardware encode (NVENC, VAAPI/AMF). Needs the `gsr-kms-server` helper for monitor capture |
| Hyprland | `slurp` selection target, and `hyprctl` for placing the camera window |
| `slurp` | Interactive region selection |
| `mpv` | Live camera preview, opened as a borderless window with class `WebcamOverlay` |
| PipeWire + WirePlumber | `-a default_output` / `default_input` audio routing |
| `ffmpeg` | Trims the start of the finished file |
| `jq` | Reading monitor and window geometry from `hyprctl -j` |
| `wl-clipboard` (`wl-copy`) | Puts the saved path on the clipboard |
| `libnotify` (`notify-send`) | Start/stop and error notifications |
| `nautilus` | Reveals the saved file (`xdg-open` works too, but selects nothing) |
| `procps`, `coreutils` | Process checks and file handling |

A camera device at `/dev/video0` (v4l2 index 0) is required for the `face` and
`message` modes. `screen` mode works without one.

### The one portability trap: KMS permissions

Recording a monitor or a region needs DRM/KMS access (upstream documents this
for AMD/Intel), which gpu-screen-recorder isolates in the separate
`gsr-kms-server` helper. The script refuses to start unless that helper exists,
because a missing helper otherwise means a password prompt on every recording or
a silent failure. It looks at `/run/wrappers/bin/gsr-kms-server` (the NixOS
location) and honours `HYPR_RECORD_KMS_SERVER` for anything else:

```bash
# Plain Fedora/Arch/Debian: grant the capability once, then point the script at it.
sudo setcap cap_sys_admin+ep /usr/bin/gsr-kms-server
```

```ini
# hyprland.conf, when the helper is not in /run/wrappers/bin
env = HYPR_RECORD_KMS_SERVER, /usr/bin/gsr-kms-server
```

Without the capability, gpu-screen-recorder falls back to a `pkexec` prompt; the
script still needs `HYPR_RECORD_KMS_SERVER` set to a real path to get that far.

## Install without Nix

1. Install the packages listed above (package names are consistent across
   Arch, Fedora and Debian-family; gpu-screen-recorder is in the Arch
   repositories, on Flathub as `com.dec05eba.gpu_screen_recorder`, and in
   nixpkgs).
2. Install the script. `hypr-record` in this bundle already starts with
   `#!/usr/bin/env bash` and `set -euo pipefail`; the Nix build normally
   supplies both, and the script depends on errexit for its failure and lock
   handling, so add them yourself if you copy `record.sh` from the repo:

   ```bash
   install -Dm755 hypr-record ~/.local/bin/hypr-record
   ```

3. Add the binds to `~/.config/hypr/hyprland.conf`, then the window rule. The
   rule keeps the camera preview floating, pinned above the desktop and
   unfocused, so it lands in the recording instead of stealing focus or being
   tiled:

   ```ini
   bind = SUPER, V, exec, hypr-record
   bind = SUPER ALT, V, exec, hypr-record face
   bind = SUPER CTRL, V, exec, hypr-record camera
   ```

   Use an absolute path if `~/.local/bin` is not in the environment Hyprland
   starts with.

   ```ini
   # Hyprland 0.55 block syntax
   windowrule {
     name = webcam-overlay
     match:class = ^WebcamOverlay$
     float = true
     pin = true
     no_initial_focus = true
   }

   # Pre-0.55 spelling, if your Hyprland has no block rules yet
   # windowrulev2 = float,class:^(WebcamOverlay)$
   # windowrulev2 = pin,class:^(WebcamOverlay)$
   # windowrulev2 = nofocus,class:^(WebcamOverlay)$
   ```

4. Reload Hyprland, press `Super+V`, drag a region, press `Super+V` again.

The script is self-checking in two places, which is useful before trusting a
full recording run:

```bash
hypr-record box 1920 1080 0 0   # prints the camera square: x y side
hypr-record status; echo $?     # 0 only while a recording is running
```

## Install with NixOS + Home Manager

System side, which is what creates the promptless `/run/wrappers/bin/gsr-kms-server`:

```nix
programs.hyprland.enable = true;
programs.gpu-screen-recorder.enable = true;
```

Home Manager side. `record.sh` is read straight out of the repo, so the script
has one source of truth:

```nix
{ pkgs, ... }:
let
  record = pkgs.writeShellApplication {
    name = "hypr-record";
    runtimeInputs = with pkgs; [
      coreutils ffmpeg gpu-screen-recorder hyprland jq libnotify
      mpv nautilus procps slurp wl-clipboard
    ];
    text = builtins.readFile ./record.sh;
  };
in
{
  wayland.windowManager.hyprland.settings.bindd = [
    "SUPER, V, Screenrecording, exec, ${record}/bin/hypr-record"
    "SUPER ALT, V, Screenrecording with camera, exec, ${record}/bin/hypr-record face"
    "SUPER CTRL, V, Video message, exec, ${record}/bin/hypr-record camera"
  ];
  # Or the equivalent entry in your windowRules list:
  # { name = "webcam-overlay"; "match:class" = "^WebcamOverlay$";
  #   float = true; pin = true; no_initial_focus = true; }
}
```

`bindd` instead of `bind` only matters if you list binds in a keybinding
chooser; `bind` works the same.

### Optional Waybar indicator

The script sends `SIGRTMIN+8` to Waybar on every start and stop, so a custom
module can show a dot while recording and stop it on click:

```json
"custom/recording": {
  "exec": "printf '%s' '●'",
  "exec-if": "hypr-record status",
  "interval": "once",
  "signal": 8,
  "tooltip": "Recording · Super+V to stop",
  "on-click": "hypr-record"
}
```

Add `custom/recording` to `modules-left` and make sure the signal appears in the
`signals` list of the modules that should refresh on it.

## Behaviour worth knowing

- Region dimensions are rounded down to even pixels; encoders reject odd ones.
- The camera square is a quarter of the shorter side of the selection, clamped
  to 160-320px, inset 24px from the bottom-right.
- The camera opens at 1280x720 MJPEG 30fps through mpv, falling back to an
  unparameterised open if that format is rejected, then waits 0.4s so the
  recording does not catch the window sliding into place.
- Audio degrades rather than failing: system + mic, then mic only, then silent
  for `face` mode; system, then silent for `screen` mode.
- Stop is `SIGINT`, escalated to `SIGKILL` after 5s with a warning notification.
  A file that is missing or empty produces a critical notification instead of a
  silent no-op.
- A lock directory guards concurrent runs. An empty lock with no live recorder is
  treated as a leftover from a crashed run and reclaimed.
- Encoding falls back to CPU (`-fallback-cpu-encoding yes`) when the GPU encoder
  is unavailable, so an unsupported GPU still records, just expensively.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| "GPU capture is not enabled yet" | `gsr-kms-server` not found at the expected path. Install it and set `HYPR_RECORD_KMS_SERVER`, and `setcap` it to avoid prompts |
| "No camera was found" | No `/dev/video*` with v4l2 index 0, or the user lacks permission (add to the `video` group), or another app holds it |
| "The camera did not open" | mpv missing, or the window rule is absent so the window got tiled and never matched `WebcamOverlay` |
| Recording stops immediately, nothing saved | Check `$XDG_RUNTIME_DIR/hypr-record/log`; usually an encoder the GPU lacks. Test `vainfo \| grep VAEntrypointEncSlice` or NVENC availability |
| Video has no sound | `-a default_output` and `default_input` resolve the PipeWire defaults, so nothing is recorded until a default sink and source exist. `wpctl status` (from `pipewire`/`wpctl`) shows both; check the microphone is not muted in your volume control |
| Camera appears in the preview but not the video | The overlay is on a different monitor or the region does not cover it; the square is placed inside the dragged region, not on screen |
| Black frames on NVIDIA under Wayland | A gpu-screen-recorder and driver combination issue, not a script one; see the upstream README |

## Source of truth

In this repo: `home/config/hyprland/record.sh` (script),
`home/user/hyprland.nix` (derivation, binds, window rule, Waybar module),
`system/modules/hyprland.nix` (`programs.gpu-screen-recorder.enable`),
`system/modules/workstation.nix` (PipeWire). This document is the
machine-independent copy; the repo files win if they disagree.

Regenerate the shareable bundle from the repo root:

```bash
rm -rf /tmp/hypr-record && mkdir -p /tmp/hypr-record
cp docs/hypr-record.md /tmp/hypr-record/README.md
{ printf '#!/usr/bin/env bash\nset -euo pipefail\n'; cat home/config/hyprland/record.sh; } \
  > /tmp/hypr-record/hypr-record
chmod 755 /tmp/hypr-record/hypr-record
tar -czf hypr-record-bundle.tar.gz -C /tmp hypr-record
```

Check the bundle before sending it: `bash -n hypr-record`, then
`./hypr-record box 1920 1080 0 0` should print `1626 786 270` and
`./hypr-record status` should exit 1 with no recording running.
