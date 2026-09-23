# GPU inventory

Checked on 2026-09-07. Live observations and declarations are separated below:
the checked-out configuration does not prove what an offline machine is running.

| Host | GPU evidence | Driver and memory | Voice scope |
| --- | --- | --- | --- |
| Andromeda | Live Vulkan runtime: AMD Radeon RX 7600, RADV NAVI33. PCI `1002:7480`. | Live `amdgpu`; about 8 GiB dedicated VRAM. | Enabled and tested with Whisper and Qwen3 Samantha on Vulkan. |
| Foundation | Declared Framework 13 AMD AI-300 Series. Exact GPU SKU unverified while offline. | Evaluated configuration enables Mesa Vulkan and early `amdgpu`. Memory unverified. | Voice services and hotkeys configured; activation, live hardware and inference checks pending. |
| Starfish | Declared Dell Precision 5750 with Intel CPU. Exact GPU hardware unverified while unreachable. | Evaluated configuration enables Mesa, with generic modesetting and no NVIDIA/PRIME configuration. | Select its GPU backend only after a live probe. |
| Terminus | Live Intel N150 integrated GPU, PCI `8086:46d4`. | Live `i915`, using system memory. | Excluded, as requested. |
| Relay | Live Apple M4 Pro, 20 GPU cores, Mac mini `Mac16,11`. | Metal 4 reported; 64 GB unified system memory. | Excluded, as requested. |

Andromeda's kernel reports 8,573,157,376 bytes of VRAM and a separate system
memory allocation budget. That budget is not additional dedicated GPU memory.
The CPU is a Threadripper 7960X with 128 GiB installed system memory.

Terminus has approximately 16 GiB system memory. Its running system exposes
Mesa Vulkan manifests, although this checkout evaluates its headless NixOS
configuration with `hardware.graphics.enable = false`. The files in a running
generation and the current declarations can differ; no change was made to
Terminus during this inventory.

Foundation was offline in Tailscale and SSH returned no route to host. Starfish
was absent from the available Tailscale peers and its hostname did not resolve.
Their exact graphics devices must not be inferred from a laptop model name,
because product lines can have different CPU/GPU options.

## Configuration evidence

- `system/foundation/configuration.nix` imports
  `nixos-hardware.nixosModules.framework-amd-ai-300-series`. The pinned module
  enables graphics, 32-bit graphics support, modesetting and `amdgpu` in the
  initrd. Its AMD parameters include `amd_pstate=active` and
  `amdgpu.dcdebugmask=0x10`.
- `system/starfish/hardware-configuration.nix` declares Intel virtualization
  and microcode support. This establishes the CPU vendor, not the GPU model.
- `system/modules/workstation.nix` supplies the shared desktop and PipeWire
  configuration used by the three workstations.
- `system/modules/server.nix` defines the headless Terminus role.
- `flake.nix` maps Relay to the `william-darwin` Home Manager configuration on
  `aarch64-darwin`.
- Fresh evaluation selected Mesa 26.1.8 for the three Linux workstations.

## Voice implications

The current speech package uses Vulkan and disables native CPU specialization,
so it can be shared between the x86_64 Linux workstations. Voice services and
Hyprland hotkeys are enabled for Andromeda and Foundation. Foundation's declared
AMD driver supports the Radeon Vulkan manifest used on Andromeda. Its Home
Manager configuration still needs activation on the laptop, followed by its
own model downloads, memory checks, inference measurements and microphone
trial. Local builds do not establish working acceleration on an offline host.

Do not apply the Radeon-specific `VK_DRIVER_FILES` setting to Starfish without
identifying its GPU. Its declared generic desktop driver does not establish
whether Intel, NVIDIA or another Vulkan backend should be selected.

The voice assets and small reference recordings can live in dotfiles. Large
model weights are downloaded separately on each enabled host and verified
against pinned hashes.

## Repeat live checks

On Linux, enumerate graphics-class PCI devices under
`/sys/bus/pci/devices`, reading `class`, `vendor`, `device` and the `driver`
symlink. AMD devices also expose `mem_info_vram_total` and
`mem_info_gtt_total`. Inspect available Vulkan manifests under
`/run/opengl-driver/share/vulkan/icd.d`, then verify actual inference or run
`nix shell nixpkgs#vulkan-tools -c vulkaninfo --summary` on the target host.
Manifest presence alone does not establish working acceleration.

On Relay, `system_profiler SPHardwareDataType SPDisplaysDataType -json`
reports the chip, GPU cores, Metal support and unified memory. Exclude serial
numbers and hardware UUIDs from any saved inventory.
