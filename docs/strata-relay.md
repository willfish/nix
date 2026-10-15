# Strata for Relay's Hermes Qwen topic

Relay runs Qwen3.8-Flash-Next IQ3_S through Strata's native Metal engine.
Hermes keeps the existing Qwen topic and `qwen-local` provider, using the
key-authenticated API at `http://127.0.0.1:8081/v1`. This is local-only;
it does not replace Tailscale or publish an unauthenticated model on the LAN.
Other hosts keep their existing inference configuration.

IQ3_S favors model quality over speed. On the 64 GiB M4 Pro its experts
stream from the internal SSD into a bounded memory arena. The larger Unsloth
4-bit model needs substantially more disk during repacking and streams more
weights. No throughput figure is assumed for this hardware. The model uses
a 32K context, high reasoning effort, and Hermes compression at 24K tokens.
The optional expert-skipping speed shortcut is disabled.

## Prepare and start

`home/user/strata-relay.nix` pins the reviewed upstream checkout. Run
`strata-relay-setup` to build Metal and download/prepare the model. It uses
Apple's Command Line Tools and their SDK, not the older SDK in a Nix shell.
Python and dependencies stay in the private Strata checkout; no Homebrew
installation or system Python changes are needed.

Source lives at `~/.local/opt/Strata`; model files and the streaming expert
copy live at `~/.local/share/strata`. Allow roughly 140 GB for preparation.
Keep them on the internal SSD. Do not prepare/repack weights beside a running
engine, or override the upstream memory caps to chase throughput.

After preparation, run `hmswitch` to install the `org.nixos.strata` system
job. `strata-relay-server` is also available for a foreground run, but do not
start a second copy while launchd owns the model. The server reads the existing
`~/.config/local-llm/api-key` at runtime, never through command arguments or
the Nix store. Hermes and Tailscale remain separate services.

`GET /health` checks model readiness. Authenticated clients use `/v1/models`
and `/v1/chat/completions`. The engine log is
`~/.local/opt/Strata/strata-iq3_s.log`; the system job log is
`/var/log/dotfiles/strata.log`. The old llama-server and its browser-tool helper
are not required for Hermes's Qwen topic.
