#!/usr/bin/env bash
set -euo pipefail

herdr_bin="$1"
dynamic_linker="$2"
library_path="$3"
installed="$("$herdr_bin" plugin list --json)"

while IFS= read -r plugin_root; do
  # Include locally linked plugins as well as GitHub installations. Only inspect
  # launchable files, not Cargo's intermediate artifacts or build scripts.
  for binary in "$plugin_root"/bin/* "$plugin_root"/target/release/*; do
    [[ -f $binary && -x $binary ]] || continue
    interpreter="$(patchelf --print-interpreter "$binary" 2>/dev/null)" || continue
    [[ $interpreter == /nix/store/* ]] || continue

    # Cargo builds outside the store do not keep their Nix runtime alive. Use
    # the runtime retained by this Home Manager generation, without a compiler.
    if [[ $interpreter != "$dynamic_linker" ]]; then
      patchelf --set-interpreter "$dynamic_linker" "$binary"
    fi
    rpath="$(patchelf --print-rpath "$binary")"
    case "$rpath" in
    "$library_path" | "$library_path":*) ;;
    *) patchelf --set-rpath "$library_path${rpath:+:$rpath}" "$binary" ;;
    esac
  done
done < <(jq -r '.result.plugins | map(.plugin_root) | unique[]' <<<"$installed")
