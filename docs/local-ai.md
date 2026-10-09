# Local AI on the workstations

Open **Local AI** from the application launcher or Waybar, or run `hypr-local-ai`.
The pinned Omarchy plugin runs in its own Quickshell host; it does not install
Omarchy or replace Waybar. Each panel manages only the machine it runs on.

The catalogue is hardware-specific. Foundation currently gets the LFM2.5-2.6B
CPU recipe, not Radeon 860M acceleration. Andromeda gets RTX 5090 recipes.
Browsing does not download weights or start models. **Download** and **Start**
do, and model images and weights can be large. Docker access is root-equivalent.

## Native Strata on Andromeda

The separate **Native Strata** section controls the existing `local-llm.service`
for Qwen3.8 Flash-Next, UD-Q4_K_XL, with 128K context. It reuses the existing
weights and API key. Start, stop, service log and Open Pi are separate actions.
If the preparation marker is absent, Prepare weights opens `strata-fetch` in a
terminal; this is a large download and conversion, not part of activation.

Native Strata is independent of Docker. The catalogue's Remove download actions
cannot remove its weights, and its activity is not included in Docker usage
charts. Stop an existing GPU model explicitly before starting another one.
Foundation can still use the existing Andromeda provider in Pi, but this panel
is not a remote service manager.

## Deployment and maintenance

Build each host's Home Manager configuration and activate with `hmswitch` on
that host. Andromeda also needs its NixOS configuration rebuilt and activated:
`hardware.nvidia-container-toolkit.enable` supplies Docker's NVIDIA CDI devices.
A Home Manager switch alone does not enable GPU containers. Check
`omarchy-local-ai readiness` before starting a catalogue model.

Agents and plugin code update through Nix, never the plugin's imperative
updaters. Catalogue refreshes remain enabled, scoped to the pinned plugin
revision; weights and runtime state remain in writable user directories.
For panel failures use `journalctl --user -u hyprland-local-ai.service`.
