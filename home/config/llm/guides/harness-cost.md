# Harness cost maintenance

Read this only when changing harness instructions, tools, extensions, skill
discovery or payload budgets. For prose changes, apply
`~/.agents/guides/documentation-relevance.md`.

## Keep capability, reduce exposure

- Distinguish local implementation code, reference files and catalogue storage
  from model-facing instructions, tool descriptions/schemas and tool results.
  File size or line count alone does not measure model context cost.
- Keep safeguards, approval gates, secret protections and manual-only triggers.
  Preserve skill discovery, explicit invocation and full on-demand body loading;
  catalogue matches never authorize actions. Prefer conditional references to
  always-loaded detail, not deleting capabilities to meet a ceiling.
- Read only installed harness documentation relevant to the changed contract,
  including required API dependencies and cross-references. Do not bulk-load docs.

## Measure the boundary and the workflow

- Measure the final provider request after harness transformations, including
  instructions, every advertised tool schema, messages and returned tool content.
  Separate fixed startup cost from complete representative workflow totals.
- Compare identical model/provider/settings, deployed generation and fixtures.
  Exercise ordinary coding, skill discovery followed by body loading, explicit
  manual-only invocation, MCP discovery/use and team delegation through completion.
  Include retries, extra discovery turns and failures, not just the first request.
- Report cold and warm catalogue/provider-cache behaviour separately. Distinguish
  characters/bytes, tokenizer counts, provider-reported cached/uncached tokens and
  actual billing; a smaller character fixture is not a measured billing saving.
- Use synthetic fixtures first. Keep any sensitive capture private, outside Git;
  retain metadata/counts only, never credentials, prompt bodies or tool contents.

## Verify the candidate manually

From the candidate dotfiles worktree, stage only intended source files so Nix's
Git flake includes new files. Inspect its `.envrc` before allowing direnv. Build
without activating; select an explicit Home Manager attribute for the target
host. Inspect the built generation rather than whichever generation is active.
Keep optional migration fixtures alongside their program and run them manually;
do not register flake, package-build or commit-hook test gates.

Preserve fail-safe catalogue behaviour on missing registration, ambiguity and
upstream/version drift: never silently strip discovery without a working
replacement. Exercise exact names, pagination, manual-only metadata, body loading,
namespace registration, enable/disable/reload, restored sessions, read truncation
and extension composition. A smaller payload does not excuse inaccessible tools
or weaker roles. Investigate changed payload ceilings rather than rubber-stamping
new snapshots; retain a capability/cost rationale.

From the dotfiles checkout, run
`direnv exec . nix shell .#repo-tools -c audit-skills --root "$PWD"` when changing
shared skills. Verify deployed guide links and unchanged skill metadata. Shared guides
deploy recursively via `home/user/llm-harness.nix`; no new always-loaded registry
entry is needed.
