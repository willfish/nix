# Local Qwen on Relay and Andromeda

Home Manager manages a Metal-accelerated llama.cpp server on the M4 Pro Mac mini
`relay` and a CUDA server on Andromeda's RTX 5090. Both use the lean `qwen-pi`
launcher and pinned Unsloth Qwen3.8-27B weights: Q6 on Relay, Q5_K_M on
Andromeda. Configuration is in
`home/user/local-llm.nix`. The browser UI and Hermes setup below apply to Relay;
Andromeda exposes its authenticated API only on localhost.

## Chat

Run `local-chat` or open <http://127.0.0.1:8081>.
From another device on the LAN, open <http://relay.fritz.box:8081>.
Alternatively, use <http://192.168.178.55:8081> (relay's
current address). If DHCP changes the address, also update the allowed browser
origins in `home/user/local-llm.nix` and run `hmswitch`, or use `relay.fritz.box`.

Run `local-chat-key` on relay to copy the login key, then paste it into the chat
page's API-key prompt. The key is stored locally with mode 0600 at
`~/.config/local-llm/api-key`, never in Git or the Nix store.

In the message box, click **+** ("Add files, prompts, tools or MCP Servers"),
then **MCP Servers**, then select **Local assistant**. The connection is configured
as "Web and files", but discovery replaces its display name with "Local assistant".
Configuring or testing it on the sidebar's MCP Servers page does not enable it
for a conversation. When a tool requests permission, click **Allow once**.
If a conversation repeatedly denied access before tools were enabled, start a
new chat with Local assistant enabled. In a controlled test, the old denial-heavy
history caused another denial despite tools being supplied, while the same
question with fresh history called `filesystem_scope` successfully.
This connection provides
internet search, reading web pages, listing and searching files, reading text
files, and writing text files in a dedicated workspace.

The assistant can read `~/Repositories`, `~/Notes`, `~/.dotfiles` and
`~/LocalAssistant`. It can only write in `~/LocalAssistant`. Common credential
paths such as `.ssh`, `.aws`, `.env*`, `secrets`, `.pem` and `.key` are excluded,
but this is not a guarantee that arbitrary documents contain no secrets. There
is no shell execution tool. Web searches leave the machine for search providers;
file contents otherwise remain in the local model and browser unless included
in a search query. Treat instructions found in files and web pages as untrusted.

The default model is Unsloth Qwen3.8-27B UD-Q6_K, a 22.0 GB dense GGUF.
Gemma 4 26B-A4B Q6 weights are retained locally, but are not loaded. The older
Qwen3.6-35B-A3B Q4_K_S weights, their Ollama hard link, and their exclusive
Ollama projector/config/manifest were removed with permission to make room.
The separate Ollama Qwen3-Coder model is unchanged.

For comparison, Gemma measured on relay on 2026-09-05 using llama.cpp 9190
achieved 39.5-44.4
generated tokens/sec for 128-token responses and 600.3 prompt tokens/sec for an
uncached 5,250-token prompt. The process used about 23.5 GiB RSS with the 64K
slot allocated and no system swap. A filesystem tool round trip succeeded and
reused 428 cached prompt tokens on the follow-up. These short tests are about
4-5 times faster at generation than the previous Qwen model, not a full-context
or model-quality evaluation.

Qwen3.8-27B Q6 rechecked on relay on 2026-09-06 using llama.cpp
9190 achieved 9.3-9.7 generated tokens/sec
for 128-token responses, and 113.2 prompt tokens/sec for an uncached 5,250-token
prompt (46.4 seconds prefill). The process used about 23.1 GiB RSS with the 64K
slot allocated and no
system swap. These are small text-chat measurements, not a full-context or
model-quality evaluation. A filesystem tool round trip succeeded and reused
888 cached prompt tokens on the follow-up.
The server is configured with one 65,536-token conversation slot, including
input and
output, with Flash Attention and q8_0 K/V caches. Parallel conversations queue.
Qwen uses its embedded chat/tool template with `preserve_thinking` enabled.
Cached browser summaries are invalidated automatically when the model changes.

Thinking is off by default for direct replies. API callers can enable it for
harder questions with `"chat_template_kwargs":{"enable_thinking":true}` and
use the model's recommended thinking settings: temperature 1.0, top_p
0.95, top_k 20 and presence_penalty 0. Non-thinking server defaults are temperature
0.7, top_p 0.8, top_k 20 and presence_penalty 1.5. Both use min_p 0 and repetition
penalty 1.0. Reset any browser sampling overrides left over from Gemma. The browser
also offers generation settings. Text chat is configured; speech and image input
are not enabled.

The server releases the model and KV cache after ten idle minutes. Sending a new
message reloads it, so the first response after sleeping takes longer. Browser
history is local to the browser profile. The service listens on all IPv4
interfaces, including localhost, LAN and Tailscale. API and tool-proxy requests
require the login key (health and model-list metadata remain public).
HTTP is unencrypted, so use only trusted networks and do
not forward this port to the internet. The tool service itself listens only on
`127.0.0.1:8082`; the authenticated chat server proxies browser tool calls to it.

## Automatic context compaction

This section describes browser chat. Hermes has its own compression policy,
described below under **Local agent in Hermes**.

The Home Manager-managed UI summarises older text messages locally before a
request exceeds 75% of the active model's context window: 49,152 tokens with the
current 65,536-token slot. Explicit output limits can trigger it earlier. It
reads the server's current model and context settings, so changing model weights
does not require rewriting the policy.

A visible notification says "Summarising older messages locally" and shows the
number of older messages processed. **Stop** cancels it. A completion notification
confirms when normal generation resumes. Reload an already-open tab after
activation to load the custom UI.

The Gemma switch was verified in an isolated browser with a deliberately lowered
compaction threshold: visible progress, a saved local summary, a shortened next
prompt, preservation of original history and recall of a fact from older history
all passed. The production context remains 65,536 tokens.

Original chat messages remain in browser history. Only the model's prompt is
shortened: it retains instructions, the current user request, recent messages and
complete tool-call/result batches, plus a continuity summary. The summary is
stored with the conversation and reused until more compaction is needed. Edits
to covered history, branch changes that alter that prefix, or changed model,
template, tool schemas or instructions invalidate the cached summary. Deleting
the conversation also deletes its summary.

Summaries are lossy and may omit details. Large initial compactions can take
several minutes on Q6 because the same local model processes the older history.
Oversized indivisible messages, incomplete tool batches and failed or truncated
summaries produce an error without deleting history. This is currently text-only.
It applies to this web UI, not arbitrary API clients, and does not remove the
separate browser tool-turn limit or force the model to continue after a final
answer. One exceptionally long response can still exhaust the remaining window.
Leave **Pre-fill KV cache after response** off (the upstream default):
that separate optimisation sends the full original history and can needlessly
replace the compacted KV cache with a much larger prompt.

`home/user/local-llm-ui.nix` builds the UI from the same pinned llama.cpp source
and npm dependencies as the runtime. Its small integration patch must apply and
type checks and upstream unit tests must pass before a new UI can build. No npm
dependencies were added and dependency install scripts are disabled. The pinned
upstream dependency audit currently reports 9 advisories (4 high, 4 moderate,
1 low); this change does not upgrade those dependencies or claim a clean audit.
The deployed artifact is static browser assets, not the upstream Node server.

To return to the stock UI, remove `--path ${chatUi}` from the server wrapper in
`home/user/local-llm.nix`, rebuild and run `hmswitch`, then reload the tab. Existing
messages remain compatible with the stock UI.

Policy and transport tests:

```sh
direnv exec . node --experimental-strip-types --test tests/local-chat-compaction*.test.mjs
```

## Lean local agent in Pi

The `pi-coding-agent` package is in the shared `home/user/packages.nix` list,
so plain `pi` is installed on every Home Manager host after updating this
checkout and switching there. The local server and `qwen-pi` wrapper are enabled
on Relay and Andromeda. Installing Pi elsewhere does not download Qwen or start
a server.

Run `qwen-pi` from the directory you want it to work in. It uses the local
Qwen server with a short system prompt and four tools: `read`, `bash`, `edit`,
and `write`. On Relay, it shares the server and API key with Hermes;
`qwen` still launches Hermes there.

```sh
qwen-pi                         # Medium thinking, interactive terminal UI
qwen-pi --thinking off          # Quick chat without thinking tokens
qwen-pi --thinking low          # Lighter reasoning
qwen-pi --thinking high         # Maps to Qwen's xhigh reasoning
qwen-pi --thinking max          # Also maps to xhigh on Pi versions with max
qwen-pi --continue              # Continue the last session in this directory
```

Home Manager installs Pi from the pinned Nix package (0.85.1 verified on
Andromeda), without a global
npm install or install-script bypass. The launcher uses an isolated agent
directory at `~/.config/local-llm/pi`. Model credentials are resolved from the
existing key file at request time, not embedded in Git, process arguments or
the Nix store. Model ID follows `modelAlias` in the Home Manager module.

The lean launcher disables automatic context-file, skill, extension, prompt
template and theme discovery. It explicitly loads only the local Qwen request
adapter. Consequently it does not inherit the shared AGENTS.md/skills harness.
Pi's own startup network checks and telemetry are disabled. This is not a
network sandbox: shell commands can still access the network, and tools run as
William with his filesystem permissions and without per-command approval
popups. The short prompt asks before destructive actions, but is not an
enforced permission boundary. No web-search tool or MCP integration is added.

Pi's native automatic compaction reserves 16,384 tokens for output and keeps
an 8,192-token recent-history budget:

| Host | Weights | Total context | Compaction above |
| --- | --- | --- | --- |
| Relay | UD-Q6_K | 65,536 | 49,152 |
| Andromeda | UD-Q5_K_M | 131,072 | 114,688 |

The context includes instructions, tool definitions, history and output.
`/compact` also requests compaction manually. Summaries use
the local model and are lossy; original sessions remain in Pi's session files.
This policy is separate from browser and Hermes compression. Home Manager
restores the declared Pi settings on activation, backing up changed settings
beside `settings.json` first. Model and prompt files remain declarative.

Measured on relay on 2026-09-06 with the production `qwen-pi` launcher:

| Task | Thinking | Cache state | Elapsed |
| --- | --- | --- | --- |
| `Hey` | Medium | Cold, 1,095 input tokens | 16.1 s |
| `Hey` repeated | Medium | 1,091 cached tokens | 4.6 s |
| `Hey` | Off | 1,091 cached tokens | 1.8 s |
| Terminal fixture diagnostic | Medium | Partially warm | 23.4 s |

Cold greeting prefill took 9.7 seconds and generation 4.1 seconds, with the
remaining time in startup/model wake overhead. The diagnostic actually ran
`pwd; uname -m; cat service.json`, identified port 80810 as invalid and suggested
8081, without changing files or using the network. Generation remained about
9.7-10 tokens/sec. This is a prompt-overhead improvement, not a faster model.
For context, stripped-down Hermes previously sent about 5,000 tokens and took
roughly 45 seconds for cold prefill alone. These are small smoke tests, not
model-quality tests or full-context endurance results.

Live requests verified thinking on/off and medium reasoning kwargs. Tests
cover reasoning-level mapping and sampling. Pi's installed compaction predicate
was checked at 49,152/49,153 tokens; a complete 64K Pi compaction session has not
been exercised.

On Andromeda, the installed Pi wrapper completed a public-fixture read-tool
round trip in 9.43 seconds. Manual compaction took 33.78 seconds, reducing
10,933 tokens to an immediate estimate of 8,539; subsequent recall preserved
both test markers. That isolated test used a temporary 32K server slot. It
verifies the tool and manual compaction paths, not a full automatic compaction
at the production limit. The summary also suggested an unrequested next step,
so successful recall does not establish complete summary fidelity.

```sh
direnv exec . node --test tests/local-pi.test.mjs
```

### OpenAI / Astra in Pi

Use plain `pi` for cloud models, not the isolated `qwen-pi` profile.
Home Manager deploys credential-free Astra definitions to
`~/.pi/agent/models.json` on all machines. Pi 0.75.4's built-in catalogue does
not include Astra, so these entries make the exact `gpt-6-astra` model selectable.
They do not grant account access, and a live Astra response still needs login
and the account's model entitlement.

For ChatGPT subscription access:

1. Run `pi` and enter `/login`.
2. Choose **ChatGPT Plus/Pro (Codex Subscription)** and complete the browser login.
3. Use `/model` and choose `openai-codex/gpt-6-astra`, or restart with:

   ```sh
   pi --provider openai-codex --model gpt-6-astra --thinking medium
   ```

Over SSH, open Pi's login URL in the browser on your desktop. If the browser
cannot reach the callback on the SSH host, paste the full final redirect URL
into Pi's authorization prompt in the terminal. Do not paste that URL or its
code into chat. Pi supports this manual fallback; no credential copying from
Codex is needed. Log in separately on each machine. Tokens live in Pi's private
`~/.pi/agent/auth.json` and are not managed by Home Manager.

For separately billed OpenAI API access, choose **OpenAI** in `/login` and enter
an API key, or supply `OPENAI_API_KEY` in the environment. Then use:

```sh
pi --provider openai --model gpt-6-astra --thinking medium
```

OpenAI API billing is separate from ChatGPT subscription usage. Cloud requests
send prompts and tool results to OpenAI, unlike the local Qwen profile.
The [official Astra model page](https://developers.openai.com/api/docs/models/gpt-6-astra)
documents a 1,050,000-token API context window. The subscription entry uses a
conservative 272,000-token client limit pending account-specific verification.
This pinned Pi exposes reasoning through `xhigh`, not Astra's additional `max`
level. Off/minimal are disabled for Astra. API cost estimates use standard
short-context rates; Pi 0.75.4 does not model Astra's higher rates above 272K
input tokens, so its long-context cost display can underestimate the bill.
See [OpenAI authentication](https://learn.chatgpt.com/docs/auth) for the
subscription/API distinction. No live Astra inference is claimed until login
has been completed and tested.

## Install and activate

On Relay:

```sh
direnv exec . nix build .#homeConfigurations.william-darwin.activationPackage --no-link
direnv exec . hmswitch
local-llm-fetch
launchctl kickstart "gui/$(id -u)/org.nix-community.home.local-llm"
local-chat
local-chat-key
```

On Andromeda:

```sh
direnv exec . nix build \
  '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . hmswitch
local-llm-fetch
systemctl --user start local-llm
qwen-pi
```

The Andromeda runtime is llama.cpp 9190 with CUDA 12.9, compiled for the RTX
5090's Blackwell architecture. It uses one 128K slot, q8 K/V, Flash Attention,
full layer offload, a 512-token logical batch and a 128-token physical batch.
Automatic fitting is disabled so it cannot silently reduce context or move
transformer layers to the CPU. The embedding service runs on eight CPU threads
to reserve GPU memory for Qwen, speech and the desktop.

Qwen shares VRAM with speech and the desktop. The previous Vulkan speech
backend exhausted memory during concurrent and repeated requests, even when
Qwen itself could run. Andromeda now uses CUDA for TTS and applies a small
patch to release obsolete decoder graphs before allocating replacements.
This avoids overlapping two decoder arenas and clears the old CUDA graph
cache before its buffers are freed. Foundation retains its Vulkan runtime.
Model weights and the voice reference remain pinned independently of backend.

On 2026-09-08, Q5 decoded a short 128-token response at about 60 tokens/sec.
With the fixed speech runtime, a 120,029-token synthetic prompt completed in
75.6 seconds and retrieved a fact from its beginning while TTS and Whisper
ran concurrently. That run left about 2.1 GiB at its sampled peak; the final
extended speech checks reached 31,398 MiB used with 711 MiB free. Repeated
speech generated 13-18 seconds of audio in about two seconds per chunk.
160K was also tested but exhausted TTS memory, so 128K is the retained shared
configuration. These are functional and memory tests, not a coding-quality
benchmark or a guarantee for other GPU workloads.

Large prompt ingestion saturates the GPU: concurrent speech chunks took
19-25 seconds in these stress tests, including cold setup for the first chunk.
Normal speech with Qwen resident but idle remains faster than playback.

Pi compacts above 114,688 context tokens, reserves 16,384 output tokens and keeps
8,192 recent tokens. Its provider is `andromeda` locally and `relay` on the Mac;
the same request adapter controls thinking and sampling on both hosts.

The service unloads the model after ten idle minutes and reloads it on the next
request. Inspect it with `systemctl --user status local-llm` or
`journalctl --user -u local-llm -n 60`. The API is at
`http://127.0.0.1:8081/v1`; the key is in `~/.config/local-llm/api-key`.
The Pi wrapper reads it automatically without exposing it in process arguments.

`local-llm-fetch` selects the host's weights from the pinned Unsloth Hugging Face
revision `4ca720788d1e01f1bff70c033e0d0028fd02e502` and verifies SHA256:

- Andromeda Q5_K_M, 19,771,509,664 bytes:
  `2de73110cb254cbf09b54b717578dadff12ef1194e7271527e68202f39ba4bfd`
- Relay Q6_K, 21,983,677,344 bytes:
  `c9c206812fbe4ac7b76a729e25928b63f2ae89d37f69da7a71c20aec763cd436`

Verification finishes before the model becomes available. Interrupted downloads
resume on the next invocation. If an identical Ollama blob already exists,
it is verified and
hard-linked instead, consuming no additional space for the weights.

Weights live under `~/.local/share/local-llm`, outside the Nix store. Builds and
activation never download model weights. Other Ollama models are unaffected.

## API and operations

API base URL: `http://127.0.0.1:8081/v1`; model: `qwen3.8-27b`.
For LAN clients, use `http://192.168.178.55:8081/v1`.
Use the key copied by `local-chat-key` as the client's Bearer API key.
The browser runs the tool-calling loop. Plain API clients must implement their
own tool loop; enabling this service does not make bare completions use tools.

```sh
curl --fail http://127.0.0.1:8081/health
launchctl print "gui/$(id -u)/org.nix-community.home.local-llm"
tail -n 50 ~/Library/Logs/local-llm.log
launchctl print "gui/$(id -u)/org.nix-community.home.local-assistant-tools"
tail -n 50 ~/Library/Logs/local-assistant-tools.log

# Stop for this login session:
launchctl bootout "gui/$(id -u)/org.nix-community.home.local-llm"

# Start again:
launchctl bootstrap "gui/$(id -u)" \
  ~/Library/LaunchAgents/org.nix-community.home.local-llm.plist
```

Home Manager reloads changed service definitions during `hmswitch`.

## Local agent in Hermes

Run `qwen` on relay, including over SSH. This is the installed Hermes harness
using the existing `qwen` profile, not the separate Qwen Code application.
Home Manager updates this profile and its launcher without reinstalling Hermes.
At activation it copies the existing API key into the mode-0600 runtime profile,
so `hermes --yolo -p qwen` also works without the wrapper. The `qwen` launcher
additionally reads the key into an environment variable. No key is embedded in
Git or the Nix store. Restart sessions opened before this authentication change.
A fresh installation needs another `hmswitch` after the server first creates
its key if using direct `hermes -p qwen` rather than the `qwen` wrapper.

The managed defaults are:

- Qwen3.8-27B Q6 with thinking enabled and medium effort in the chat-template
  parameters. The browser still defaults to thinking off. Generic top-level
  reasoning flags alone do not control this llama.cpp version's template.
- YOLO enabled: terminal/file tool calls do not ask for approval. This is host
  access as William, not the browser MCP's restricted workspace or root access.
  Existing OS permissions and Hermes security blocks still apply.
- Terminal and file tools loaded by default. The launch directory is forwarded
  explicitly because Hermes's `-z` path bypasses its interactive cwd setup.
- Unlimited tool turns, independent of context compression. Completion, user
  cancellation and unrecoverable errors can still end a run.
- Streaming and visible reasoning enabled in interactive chat.
- Automatic compression at 49,152 tokens or earlier if an output reservation
  reduces the usable budget. The explicit token cap prevents Hermes's internal
  small-window floor from raising this to 55,705 tokens.
- Compression uses the main local model, with thinking disabled, visible progress
  and a 1,800-second timeout. No configured fallback model/provider is used.
  A failed summary stops rather than silently replacing history with a generic
  marker. Summary generation itself can take minutes for a large context.
- Automatic title generation and background review are disabled, along with
  automatic curator/triage routing. Browser web search remains separately
  available; the normal local repair toolset does not require web search.

Unrelated profile settings, memories and skills are retained. Changed YAML is
backed up privately alongside the original as `config.yaml.before-local-llm-*`;
the previous launcher is `~/.local/bin/qwen.before-local-llm`. Managed values
come from `home/user/local-llm.nix` and are reapplied by `hmswitch`.

Measurements on 2026-09-06 used a harmless terminal task: inspect a JSON fixture,
identify its invalid port and suggest a valid replacement. They do not measure
model quality or demonstrate arbitrary system repair competence.

<!-- markdownlint-disable MD013 -->

| Run | Wall time | Key observation |
| --- | --- | --- |
| Minimal isolated Hermes, thinking off, cold prompt | 54.1 sec | 5,036-token prefill took 44.7 sec; decode 9.55 tokens/sec |
| Minimal isolated Hermes, thinking on, cached prompt | 22.3 sec | Cache reused 5,032 prompt tokens; not comparable to a cold run |
| Actual `qwen` profile, thinking on, partly cached | 32.9 sec | Terminal task succeeded; decode 9.2-9.4 tokens/sec |
| Actual `qwen chat` path, thinking on, warm cache | 25.5 sec | Terminal task succeeded; decode 9.1 tokens/sec |
| Direct `hermes --yolo -p qwen chat`, partly cached | 27.5 sec | No wrapper credentials; authenticated terminal task succeeded at 9.3-9.4 tokens/sec |

<!-- markdownlint-enable MD013 -->

The initial real-profile attempt took 97 seconds and failed to find the fixture
because the terminal was pinned to the old OpenClaw directory. The profile and
launcher now propagate the launch directory. Streaming had also been disabled.
The direct and agent tests show similar generation throughput; cold prompt
processing, reasoning length and unnecessary tool turns dominate perceived delay.

A forced test of Hermes's real compressor reduced a synthetic diagnostic log
from 26 messages to 6, retained the approved repair port and latest request, and
produced a smaller transcript. This took 270 seconds using the local model.
The automatic trigger was independently checked immediately below and at 49,152
tokens. This was not a full 64K endurance run or an internet-disconnection drill.
