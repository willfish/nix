#!/usr/bin/env bash
set -euo pipefail

work=$(mktemp -d "${TMPDIR:-/tmp}/dependency-hashes.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

refresh_hash() {
	local attr=$1 file=$2 field=$3 drv old new
	drv=$(nix eval --raw "$attr.drvPath")
	if nix build --no-link --log-format raw "$drv^out" >"$work/build.log" 2>&1; then
		return 0
	fi
	cat "$work/build.log" >&2

	# Only use a mismatch from the exact dependency cache we requested.
	if ! read -r old new < <(awk -v header="error: hash mismatch in fixed-output derivation '$drv':" '
    $0 == header {
      if (++matches != 1 || (getline) <= 0 || NF != 2 || $1 != "specified:") { invalid = 1; exit }
      old = $2
      if ((getline) <= 0 || NF != 2 || $1 != "got:") { invalid = 1; exit }
      new = $2
    }
    END { if (matches == 1 && !invalid) print old, new }
  ' "$work/build.log"); then
		return 1
	fi
	local sri='^sha256-[A-Za-z0-9+/]{43}=$'
	[[ $old =~ $sri && $new =~ $sri && $old != "$new" ]] || return 1

	if ! awk -v field="$field" -v old="\"$old\";" -v new="\"$new\";" '
    $1 == field && $2 == "=" && $3 == old && NF == 3 {
      sub(/"[^"]+";/, new)
      count++
    }
    { print }
    END { if (count != 1) exit 1 }
  ' "$file" >"$work/updated"; then
		printf 'Expected one %s with the old hash in %s; not updating it.\n' "$field" "$file" >&2
		return 1
	fi
	install -m 644 "$work/updated" "$file"
	printf 'Refreshed %s: %s\n' "$attr" "$new"
}

system=$(nix eval --impure --raw --expr builtins.currentSystem)
if [[ $system == x86_64-linux ]]; then
	refresh_hash .#opencode2.nodeModules home/user/opencode-package.nix hash
fi
refresh_hash .#pi-mcp-adapter.npmDeps home/user/mcp-packages/pi-mcp-adapter.nix npmDepsHash
