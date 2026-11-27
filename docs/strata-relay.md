# Inkling on Relay

Relay serves `inkling-2-mini` through MLX on Apple silicon. Hermes keeps its
existing Qwen topic and `qwen-local` provider at `http://127.0.0.1:8081/v1`.
Pi and OpenCode use `http://relay.fritz.box:8081/v1` on the LAN. Both addresses
require the existing Relay bearer key. Port 8081 listens on all IPv4 interfaces;
keep it off the public internet.

The API offers `GET /health`, `GET /v1/models`, and `POST /v1/chat/completions`,
including streaming and text tool calls. Media inputs, model management, alternate
models and local adapters are unavailable through this endpoint. The native
MLX server listens on loopback with a separate ephemeral key.

The context limit is 65,536 tokens, including output. The server uses one
generation slot, 1,024-token prefill steps, RAM-only automatic prefix caching,
and no speculative drafting. Reasoning defaults to `medium`; clients can send
`reasoning_effort: "low"` for prose. The LAN listener starts after the managed
launcher verifies the runtime settings and the model has loaded successfully.

## Prepare and deploy

Build the Relay configuration on the Mac and run `inkling-relay-setup` from the
built Home Manager generation's `home-path/bin` before activating. This uses a
private, pinned Python and wheel environment at
`~/.local/share/inkling-relay/mlx-vlm-0.8.1`; neither system Python nor the old
Strata checkout is needed. Setup checks and reuses the staged `mlx-4bit`
directory or downloads the required files for
`VisualInference/Inkling-2-Mini-MLX-4bit` at revision
`443e30e1ddbb05b711975a6b549ffe209755edc6`. Allow about 16 GB for new model
files. If setup has to update an existing Inkling environment, stop that job
first.

Activate through the normal [Relay deployment](headless-darwin.md). The Inkling
launcher takes over the existing `org.nixos.strata` system job and its log at
`/var/log/dotfiles/strata.log`. Look for `Inkling ready` after the first model
load, then check authenticated `/health` and `/v1/models`. The job reads
`~/.config/local-llm/api-key` at startup.

## Roll back to Strata

Re-activate the previous Relay system generation to restore the launchd job and
the matching client settings together. Keep the old generation, checkout
`~/.local/opt/Strata-v0.2.11`, and prepared weights under `~/.local/share/strata`
until rollback is no longer needed. For manual recovery, the retained
`strata-relay-setup` and `strata-relay-server` commands prepare and run the
Metal backend; stop the managed Inkling job before binding port 8081 by hand.
The old API model ID was `qwen3.8-flash-next-unsloth-ud-iq4_xs-v31`.
