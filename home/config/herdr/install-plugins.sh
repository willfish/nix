#!/usr/bin/env bash
set -euo pipefail

herdr_bin="$1"
catalog="$2"
# Validate every entry before performing any installation.
jq -e 'type == "array" and length > 0 and all(.[];
  (.id | type == "string") and
  (.owner | test("^[A-Za-z0-9_-]+$")) and
  (.repo | test("^[A-Za-z0-9_.-]+$")) and
  (.revision | test("^[0-9a-f]{40}$")))' "$catalog" >/dev/null

matches() {
  jq -e --arg id "$id" --arg owner "$owner" --arg repo "$repo" --arg revision "$revision" '
    any(.result.plugins[]?;
      .plugin_id == $id and .source.kind == "github" and
      .source.owner == $owner and .source.repo == $repo and
      .source.resolved_commit == $revision)' >/dev/null
}

failed=0
while IFS=$'\t' read -r id owner repo revision; do
  installed="$("$herdr_bin" plugin list --plugin "$id" --json)" || {
    failed=1
    continue
  }
  if matches <<<"$installed"; then
    continue
  fi
  # Let Herdr stage the replacement; never uninstall a working copy first.
  if ! "$herdr_bin" plugin install "$owner/$repo" --ref "$revision" --yes >/dev/null; then
    echo "warning: could not install pinned Herdr plugin $id" >&2
    failed=1
    continue
  fi
  installed="$("$herdr_bin" plugin list --plugin "$id" --json)" || {
    failed=1
    continue
  }
  if ! matches <<<"$installed"; then
    echo "warning: Herdr plugin $id does not match its pinned provenance" >&2
    failed=1
  fi
done < <(jq -r '.[] | [.id, .owner, .repo, .revision] | @tsv' "$catalog")
exit "$failed"
