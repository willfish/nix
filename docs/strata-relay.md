# Strata for Relay's Hermes Qwen topic

Relay runs Qwen3.8-Flash-Next Unsloth Dynamic v3.1 (`UD-IQ4_XS-v31`) through
Strata's native Metal engine. Its API model ID is
`qwen3.8-flash-next-unsloth-ud-iq4_xs-v31`.
Hermes keeps the existing Qwen topic and `qwen-local` provider, using the
key-authenticated API at `http://127.0.0.1:8081/v1`. This is local-only;
it does not replace Tailscale or publish an unauthenticated model on the LAN.
Other hosts keep their existing inference configuration.

The approximately 4-bit model favors quality over speed. On the 64 GiB M4 Pro
its experts stream from the internal SSD into a bounded memory arena. It streams
more weights than the previous IQ3_S model. No throughput or benchmark-quality
gain is assumed for this hardware. The model uses
a 64K context, high reasoning effort, and Hermes compression at 48K tokens.
Hermes rejects windows below 64,000 tokens, so the launcher enforces 65,536
in a runtime copy of the setup config, including older 32K installations.
The upstream streaming-memory limits remain enabled. The optional
expert-skipping speed shortcut is disabled. Prompt processing uses 4096-token
chunks. MTP/DFlash2 drafting and the optional FP16 decode path remain disabled.

## Prepare and start

`home/user/strata-relay.nix` pins the reviewed upstream checkout. Run
`strata-relay-setup` to build Metal and download/prepare the model. It uses
Apple's Command Line Tools and their SDK, not the older SDK in a Nix shell.
Python and dependencies stay in the private Strata checkout; no Homebrew
installation or system Python changes are needed.

Source lives at `~/.local/opt/Strata-v0.2.11` (engine 0.1.57); model files live
at `~/.local/share/strata`. Setup pins Unsloth's November 4 revision
`d96201194f103c906994f283f504209dc8d2a10c` and checks shard sizes and SHA-256.
This is the four-shard v3.1 release, not the older three-shard `UD-IQ4_XS`.
Allow roughly 165 GB for the 93.7 GB download, repacked Metal GGUF and preparation
workspace. The optional SSD-read-order expert copy needs about 60 GB more, plus
10 GB free; setup skips it safely when there is insufficient room.
Keep them on the internal SSD. Do not prepare/repack weights beside a running
engine, or override the upstream memory caps to chase throughput.
Keep the previous `~/.local/opt/Strata` and `Strata-v0.2.10` installations for
rollback. If space requires moving the old IQ3_S weights off Relay, verify the
archive before deleting the local copy; restoring the old launcher alone does
not restore those weights.

After preparation, run `hmswitch` to install the `org.nixos.strata` system
job. `strata-relay-server` is also available for a foreground run, but do not
start a second copy while launchd owns the model. The server reads the existing
`~/.config/local-llm/api-key` at runtime, never through command arguments or
the Nix store. Hermes and Tailscale remain separate services.

`GET /health` checks model readiness. Authenticated clients use `/v1/models`
and `/v1/chat/completions`. The engine log is
`~/.local/opt/Strata-v0.2.11/strata-unsloth-ud-iq4_xs-v31.log`;
the system job log is `/var/log/dotfiles/strata.log`.
The old llama-server and its browser-tool helper
are not required for Hermes's Qwen topic.
