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

## Run candidate-generation checks

From the candidate dotfiles worktree, stage only intended source files so Nix's
Git flake includes new files. Inspect its `.envrc` before allowing direnv. Build
without activating; select another explicit Home Manager attribute on other hosts.
These are the self-contained payload/core checks from `.github/workflows/ci.yml`,
using Andromeda's built generation rather than whichever generation is active:

```sh
direnv exec . nix build '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . bash -euo pipefail <<'SH'
generation="$(nix eval --raw '.#homeConfigurations."william@andromeda".activationPackage.outPath')"
export PI_MCP_TEST_BIN="$generation/home-path/bin/pi"
export PI_SKILL_CATALOG_TEST_BIN="$PI_MCP_TEST_BIN"
export PI_MCP_TEST_EXTENSION="$generation/home-files/.pi/agent/extensions/mcp/index.ts"
export PI_SKILL_TEST_EXTENSION="$generation/home-files/.pi/agent/extensions/skill-catalog/index.ts"
export PI_HARNESS_TEST_HOME_FILES="$generation/home-files"
export PI_OFFLINE=1 PI_TELEMETRY=0 CAPTURE_PROMPTS=0
test -x "$PI_MCP_TEST_BIN"
test -f "$PI_MCP_TEST_EXTENSION"
test -f "$PI_SKILL_TEST_EXTENSION"
test -f "$generation/home-files/.pi/agent/extensions/reading-policy.ts"
test -d "$PI_HARNESS_TEST_HOME_FILES/.agents/skills"
test -f "$PI_HARNESS_TEST_HOME_FILES/.pi/agent/AGENTS.md"
node --experimental-vm-modules --test tests/pi-skill-catalog.test.ts tests/pi-reading-policy.test.ts tests/pi-mcp-namespace-tools.test.ts
python3 tests/pi-mcp-runtime.py -v
python3 tests/pi-harness-slim-runtime.py -v
SH
```

- Preserve fail-safe catalogue behaviour on missing registration, ambiguity and
  upstream/version drift: never silently strip discovery without a working
  replacement. Test exact names, pagination, manual-only metadata and body loading.
- Keep namespace registration, enable/disable/reload, restored sessions, read
  truncation and extension-composition regressions covered. A smaller payload
  does not excuse inaccessible tools or weaker roles.
- The static budget in `tests/pi-harness-slim-runtime.py` counts instructions plus
  serialized schemas against 23563 characters and asserts the full tool set.
  It is a deterministic regression guard, not a complete workflow or billing test.
  Investigate failures before changing ceilings; approve necessary increases with
  a capability/cost rationale, never rubber-stamp a new snapshot.
- Run `direnv exec . python3 home/config/llm/scripts/audit-skills` and normal
  repository hooks. Verify deployed guide links and unchanged skill metadata.
  Shared guides deploy recursively via `home/user/llm-harness.nix`; no new
  always-loaded registry entry is needed. Recheck CI/source commands on drift.
