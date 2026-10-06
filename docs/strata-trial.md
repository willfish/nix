# Andromeda local-model trial

Andromeda's `local-llm.service` serves Qwen3.8-Flash-Next through Strata at
`http://127.0.0.1:8081`, with the existing API key and tailnet endpoint unchanged.
The trial uses Unsloth UD-Q4_K_XL, 131,072 tokens of context and an 8-bit KV cache.
It reserves 3 GiB of otherwise free VRAM for desktop and voice workloads.

Strata and its CUDA 13 toolchain are pinned and built through Nix. Model files
live outside the Nix store in `~/.local/share/strata`. To provision them again,
run `strata-fetch`, then `systemctl --user restart local-llm`. Allow roughly
120 GB of disk space for the model, draft layer and prepared data.

## Use and compare

- Run `qwen-pi`, or select **Andromeda Qwen 3.8 Flash-Next Q4 (Strata)** in Pi.
- The browser chat is at `http://127.0.0.1:8081`. It requires the existing local
  model API key, held in `~/.config/local-llm/api-key`.
- In OpenCode, select the Flash-Next model under Andromeda.
- Only one of the two local model services can run at a time. Selecting a model
  in a client does not switch the server.

Switch back to the retained 27B model without downloading it again:

```sh
systemctl --user start local-llm-27b
qwen-pi --model qwen3.8-27b
```

Return to Strata:

```sh
systemctl --user start local-llm
qwen-pi
```

The fallback is manual. Login and Home Manager activation default to Strata.
Compare identical prompts, thinking levels and context lengths. Check time to
first output, total completion time and correctness, not only output tokens per
second. This quantisation is experimental upstream; a higher bit count does not
prove better answers than the 27B model.

## Inspect

```sh
systemctl --user status local-llm
journalctl --user -u local-llm -n 50
less ~/.local/state/strata/engine.log
nvidia-smi
free -h
```

The engine log reports prompt/decode throughput and memory allocation. A cold
start reads tens of gigabytes and can briefly make the workstation less
responsive. The 128K context limit includes both prompt and generated output.
