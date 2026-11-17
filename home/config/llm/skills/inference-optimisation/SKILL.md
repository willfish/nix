---
name: inference-optimisation
description: Local LLM inference optimisation. Use when tuning or benchmarking model servers, decode or prefill throughput, speculative decoding, KV cache, startup or TTFT, or estimating hardware throughput ceilings.
---

# Inference optimisation

Read `UPSTREAM.md` for the complete hill-climbing workflow. Its relative paths
resolve from this skill directory. Load the supporting references only as needed.
The upstream bundle, examples and scripts are pinned by
`inference-optimisation-skill-source` in the dotfiles `flake.lock`; retain its MIT
`LICENSE` when redistributing.

Before writing plans, reports or recipes, read
`~/.agents/guides/documentation-relevance.md`.

## Local boundaries

- Loading or installing this skill does not authorize benchmarks, server changes
  or publication. Stay within the requested machine, workload and resource limits.
  Ask before disrupting existing workloads, changing privileged host settings or
  making external commitments outside that scope.
- Inspect each config's `launch`, `stop` and memory-probe commands before running
  it. The bundled harness does not enforce every upstream ground rule: probe
  failures can be ignored and stopping does not guarantee a drained server.
  Verify memory-cap enforcement and graceful shutdown independently before load.
  Reject failed retrievals and flagged runs even if the script exits successfully.
- Run experiments from a separate working directory, not this read-only skill
  directory. Use the project's direnv environment; if Python is missing, use an
  ephemeral Nix shell with `python3`, not a host or project dependency install.
- Qualify results locally first. Publishing, pushing results or changing a shared
  deployment requires the applicable workflow and authorization. Follow project
  branch rules rather than automatically creating the upstream recipe branch.
