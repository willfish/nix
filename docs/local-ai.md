# Local AI on the workstations

Open **Local AI** from the application launcher or Waybar, or run `hypr-local-ai`.
The pinned Omarchy plugin runs in its own Quickshell host; it does not install
Omarchy or replace Waybar. Each panel manages only the machine it runs on.

The catalogue is hardware-specific. Foundation currently gets the LFM2.5-2.6B
CPU recipe, not Radeon 860M acceleration. Andromeda gets RTX 5090 recipes.
The **Models** tab shows the full catalogue; Home favours downloaded, pinned
and recommended models. Browsing does not download weights or start models.
**Download** and **Start** do, and images and weights can be large.
Docker access is root-equivalent.

## Strata on Andromeda

**Qwen3.8 Flash-Next (Strata)** is a recipe in the same catalogue, with the
standard Start, Stop, Open and usage display. Its Nix-built Docker image uses
the Huihui abliterated UD-Q4_K_XL weights and the existing MTP pack, with 128K
context. The weights are mounted read-only, not copied into the image or
downloaded again. The previous official Unsloth shards remain on disk with a
.official suffix.

If weights are missing, run `strata-fetch`: it checks/downloads the pinned
GGUF shards and prepares the packs inside Docker. This is a large download
and conversion, not part of Home Manager activation. Catalogue removal cannot
delete these imported weights.

The private engine sits behind the widget's authenticated gateway. Strata also
publishes that gateway on port 8081, preserving the existing Andromeda provider
and API key in Pi. Requests through either port count towards widget usage.
Other recipes use the widget's normal local ports and Open action.

Stop an existing GPU model before starting another. Docker restarts running
models after reboot unless they were stopped. The suspend launcher stops all
widget-managed GPU models before requesting sleep. Foundation can still use
Andromeda's provider in Pi, but its widget is not a remote service manager.

## Deployment and maintenance

Build each host's Home Manager configuration and activate with `hmswitch` on
that host. Andromeda also needs NVIDIA CDI enabled in NixOS:
`hardware.nvidia-container-toolkit.enable`. A Home Manager switch alone does
not enable GPU containers. Check `omarchy-local-ai readiness` before starting.

Agents and plugin code update through Nix, never the plugin's imperative
updaters. To review a newer plugin revision, run
`nix flake update omarchy-local-ai`, inspect the source/patch compatibility,
then build and activate. Registry refreshes preserve the local Strata recipe;
weights and runtime state remain in writable user directories.
For panel failures use `journalctl --user -u hyprland-local-ai.service`.
