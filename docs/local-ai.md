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

## Shared Qwen endpoint on Andromeda

Start **Qwen3.8 Flash-Next (Unsloth v3.1)** using the Nix-maintained Strata recipe:

```sh
omarchy-local-ai run qwen3.8-flash-next.strata.256k.rtx-5090-32gb nvidia:0
```

Its authenticated gateway also serves `http://127.0.0.1:8081/v1` and
`http://andromeda.taile09696.ts.net:8081/v1`. It publishes port 8081 on every
IPv4 address, so the LAN can reach it too: Docker's published ports bypass the
NixOS firewall. Every request still needs the API key. The recipe does not
depend on Tailscale. Do not enable Funnel or forward the port to the internet.
Other recipes and extra deployment instances retain only
the widget's normal local ports unless explicitly shared through the widget.

Pi and OpenCode retain the `andromeda` provider, `qwen3.8-flash-next` alias and
existing API key from `~/.config/local-llm/api-key`; the gateway maps the alias
to the recipe's served model. The clients keep their conservative 128K context
and 16K output limits, while the recipe advertises 256K. A declared server
limit is not evidence of a successful full-context run.

The recipe pins Strata v0.2.11's CUDA 13 image (engine 0.1.57) and Unsloth's
November 4 Dynamic v3.1 `UD-IQ4_XS` GGUF shards at revision
`d96201194f103c906994f283f504209dc8d2a10c`. Local AI verifies the downloads,
prepares the pack privately, then mounts both weights and pack read-only.
The engine is reachable only through the authenticated gateway. Its key is not
passed into the engine. Drafting and expert skipping are disabled; 4 GiB of VRAM
is reserved for the desktop. The recipe permits one running inference request.
Reasoning and visible output share the client's output allowance.

The former `qwen3.8-flash-next.sglang.256k.rtx-5090-32gb` recipe and its
`turboderp` EXL3 download remain available for rollback, on the widget's private
port rather than the shared endpoint. Stop Strata before starting it. Restoring
EXL3 on port 8081 also requires changing `LOCAL_AI_SHARED_RECIPE` back and
running `hmswitch`, then restarting that recipe.

Stop an existing GPU model before starting another. Docker restarts running
models after reboot unless they were stopped. The suspend launcher stops all
widget-managed models before requesting sleep. Foundation can use Andromeda's
provider in Pi, but its widget is not a remote service manager.

Relay's separate Metal service is unchanged; see [Relay Qwen](strata-relay.md).
Do not remove its service or model data as part of workstation cleanup.

## Deployment and maintenance

Build each host's Home Manager configuration and activate with `hmswitch` on
that host. Andromeda also needs NVIDIA CDI enabled in NixOS:
`hardware.nvidia-container-toolkit.enable`. A Home Manager switch alone does
not enable GPU containers. Check `omarchy-local-ai readiness` before starting.

Agents, the Strata recipe and plugin code update through Nix, never the plugin's
imperative updaters. To review a newer plugin revision, run
`nix flake update omarchy-local-ai`, inspect the source/patch compatibility,
then build and activate. If the shared recipe ID changes, update
`LOCAL_AI_SHARED_RECIPE` in `home/user/omarchy-local-ai.nix` with it.
The custom Strata recipe lives in
`home/config/hyprland/local-ai/strata-recipe.nix`; registry refreshes retain its
pinned execution contract. A recipe change gets its own catalogue cache scope.
Weights and runtime state remain in writable user directories. Builds and
activation do not download weights or restart model containers; stop/start
an existing recipe to apply gateway or engine launch changes.
For panel failures use `journalctl --user -u hyprland-local-ai.service`.
