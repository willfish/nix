# Repository data helpers

Build with `nix build .#repo-tools`. Both commands operate on data only; neither
runs Git, evaluates flake inputs, fetches repositories or executes theme code.

## Lock update policy

`check-flake-lock-update BASE HEAD` validates both complete lock graphs before
allowing changes. It rejects duplicate JSON keys, unsupported structures/source
attributes, unresolved references and follows cycles. Ordinary dependency cycles
and shared nodes remain legal. Unreachable nodes receive the same checks.

Only the existing GitHub revision/hash/timestamp fields may change. Graph and
source identity remain fixed; owner approval comes only from exact lines in
`AUTO_MERGE_GITHUB_OWNERS`, with no default allowlist. JSON booleans and integers
are not interchangeable. Rejections return 1 with `Manual review required:`;
accepted changes are sorted in the existing success message.

Build/run this checker from trusted code, not a pull-request checkout. Passing
candidate lock files to an already trusted executable is safe; evaluating a PR's
flake to build that executable is not. The command grants no merge approval by
itself and is not automatically wired to workflows or hooks.

## Community import

`import-omarchy-community [--root CHECKOUT] PAGE.html SOURCE_URL` reads a downloaded,
reviewed page. The checkout defaults to the working directory; use `--root` when
calling from elsewhere. It updates only the marked community-input block and
`home/user/themes/community.json`. Review both changes before running a flake
lock update.

HTML5 SAX tokenization preserves explicit figure boundaries instead of repairing
an incomplete document. Raw start-tag attributes retain the former last-duplicate
behavior, and entity decoding is data-only. External resources are disabled.
Slugs, GitHub URLs and labels are validated before writing; duplicate or empty
catalogues fail. Unavailable themes remain in the catalogue but not the generated
input block. Publication retains the original direct, ordered two-file writes,
not an all-files transaction.

## Manual verification

Supply GLib, yyjson, libxml2 (with HTML5 token mode), Meson, Ninja, pkg-config and
Node from a direnv/ephemeral Nix environment:

```sh
meson setup /tmp/repo-tools-build home/config/repo-tools
meson compile -C /tmp/repo-tools-build
LOCK_POLICY_BIN=/tmp/repo-tools-build/check-flake-lock-update \
COMMUNITY_IMPORT_BIN=/tmp/repo-tools-build/import-omarchy-community \
  node --experimental-strip-types --test home/config/repo-tools/*.test.ts
```

Fixtures are manual, noninstalled and unregistered. They use local lock data and
disposable checkout copies, never live imports or merges. Optional
`LOCK_POLICY_LEGACY` and `COMMUNITY_IMPORT_LEGACY` select retained original scripts;
`*_PYTHON` selects their interpreter. `COMMUNITY_IMPORT_PAGE` adds a pinned local
HTML-page comparison. No migration test is run by the package or commit hooks.
