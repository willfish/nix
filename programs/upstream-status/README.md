# Upstream status

`upstream-status [--check] [FLAKE_DIR]` compares the pinned revisions in
`flake.lock`, and the few artifacts pinned outside it, with their upstream
repositories. It is read-only: it never edits the lock, package files or the
store, and it makes no merge or update decision.

Reported inputs are the top-level GitHub inputs whose names end in `-source`
plus `hermes-agent`. Moving-branch inputs such as nixpkgs, Home Manager and
Omarchy are intentionally left out because "behind" says nothing useful about
them; `nix flake update` already owns them. Artifacts that cannot live in the
lock (npm tarballs and release binaries) are reported from their package files.

Set `GITHUB_TOKEN` to raise the GitHub API rate limit. One run uses about
thirty unauthenticated requests. `--check` exits 1 when anything reported is
behind, which suits a manual or scheduled reminder; it is not wired into the
commit hooks.

`hermes-agent` and `herdr-source` follow upstream release tags through explicit
pins in `flake.nix`, so the report compares them with the latest release rather
than the branch head. A new release needs a deliberate pin edit; `nix flake
update` will not move a commit or tag URL on its own.

Moving a source revision into `flake.lock` removes the manual `rev`/`hash`
pair, but two things stay manual after a bump:

- derived hashes: `cargoHash`, `vendorHash` and `npmDepsHash` still depend on
  the source. A build that fails with `got: sha256-...` names the package; put
  that value in the matching attribute.
- patches: rebasing is a review task. A failed build surfaces it, but the
  rebase itself is not automated.

Check the exact pinned surface with:

```bash
direnv exec . nix run .#upstream-status -- .
```
