# Deferred OpenCode MCP discovery

Build OpenCode from the `opencode-source` input, using its frozen dependency
lockfile and script-disabled Nix dependency build. The patch adds local `lazy`
and `description` MCP configuration fields. They are not upstream V2 options.

The native `mcp` tool lists all configured capabilities, searches cached tool
metadata, and connects a selected server for fresh schemas and full instructions.
Code Mode remains the executor; native permissions, OAuth and resource handling
remain in place. Lazy service calls require discovery in an earlier model step
and instruction delivery in that session. Metadata caches are advisory, scoped
to configuration, location and OAuth identity, and refreshed before operations.

Code Mode's compact index is assembled from the current executable snapshot for
each request, outside durable instruction-update history. Existing histories are
not rewritten; old catalogue messages disappear through normal compaction.

After rebasing the patch, run the affected upstream MCP, Code Mode, model-request
and instruction tests, plus core type checking. The added lazy regression covers
cold startup, metadata-only discovery, same-step and cross-session denial, and
cache/session restoration. Repeat the private Sol token fixture from
`pi-token-mitm` before accepting a new cost baseline. Never commit raw captures.

Build with `nix build .#opencode2`, then build the selected Home Manager
configuration. Follow `docs/nixos-host-operations.md` for activation.
The Linux dependency hash is tied to the selected Bun version; regenerate it
from the unchanged frozen lockfile when the build tool changes. Other platforms
require their own native build and dependency-hash verification before deployment.
