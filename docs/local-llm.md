# Local chat on relay

Home Manager manages a Metal-accelerated llama.cpp server on the M4 Pro Mac mini.
The configuration is in `home/user/local-llm.nix` and only applies to Darwin host
`relay`.

## Chat

Run `local-chat` or open <http://127.0.0.1:8081>.
From another device on the LAN, open <http://192.168.178.55:8081> (relay's
current address). If DHCP changes the address, use the new address on port 8081.

The default model is Qwen3.6-35B-A3B Q4_K_S, chosen for responsive conversation.
Its mixture-of-experts architecture activates about 3B parameters per token. This
is the faster alternative to the denser Qwen3.8-27B intelligence-first choice.
The model supports a single 65,536-token conversation slot, including input and
output, with Flash Attention and q8_0 K/V caches. Parallel conversations queue.
The template preserves prior thinking blocks, including empty ones, so subsequent
turns keep the same token prefix and can reuse the hybrid model's cached state.

Thinking is off by default for direct replies. API callers can enable it for
harder questions with `"chat_template_kwargs":{"enable_thinking":true}` and
use the model's recommended thinking sampling settings: temperature 1.0, top_p
0.95, top_k 20, min_p 0, presence_penalty 0 and repetition penalty 1.0. The browser
also offers generation settings. Text chat is configured; speech and image input
are not enabled.

The server releases the model and KV cache after ten idle minutes. Sending a new
message reloads it, so the first response after sleeping takes longer. Browser
history is local to the browser profile. The service listens on all IPv4
interfaces, including localhost, LAN and Tailscale, with no API authentication.

## Install and activate

```sh
direnv exec . nix build .#homeConfigurations.william-darwin.activationPackage --no-link
direnv exec . hmswitch
local-llm-fetch
launchctl kickstart "gui/$(id -u)/org.nix-community.home.local-llm"
local-chat
```

`local-llm-fetch` verifies and hard-links the existing Ollama blob when available.
This consumes no additional space for the weights, and the hard link remains valid
if the model is later removed from Ollama. On a fresh machine it downloads 19.9 GB
from a pinned Hugging Face revision and verifies the SHA256 before making the
model available. Interrupted downloads resume on the next invocation.

Weights live under `~/.local/share/local-llm`, outside the Nix store. Builds and
activation never download model weights. Other Ollama models are unaffected.

## API and operations

API base URL: `http://127.0.0.1:8081/v1`; model: `qwen3.6-35b-a3b`.
For LAN clients, use `http://192.168.178.55:8081/v1`.
No API key is configured. Clients requiring a nonempty key can use `local`.

```sh
curl --fail http://127.0.0.1:8081/health
curl --fail http://127.0.0.1:8081/v1/models
launchctl print "gui/$(id -u)/org.nix-community.home.local-llm"
tail -n 50 ~/Library/Logs/local-llm.log

# Stop for this login session:
launchctl bootout "gui/$(id -u)/org.nix-community.home.local-llm"

# Start again:
launchctl bootstrap "gui/$(id -u)" \
  ~/Library/LaunchAgents/org.nix-community.home.local-llm.plist
```

Home Manager reloads changed service definitions during `hmswitch`.
