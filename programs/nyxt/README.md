# Nyxt

Source-built Nyxt with the CEF renderer for x86_64 Linux. Nyxt, its Lisp
dependencies, the clipboard library and the CEF bridge are compiled locally.
Chromium itself comes from the CEF binary distribution selected by upstream.

```sh
direnv exec . nix build .#nyxt --no-link
direnv exec . nix run .#nyxt -- --no-config --no-auto-config
```

Linux desktop Home Manager profiles install the package without changing the
default browser. The package enables Chromium's sandbox rather than preserving
the CEF bridge's unsandboxed default. Do not work around a startup failure with
`--no-sandbox`.

## Updating

`flake.lock` pins `nyxt-source`. Update it with
`direnv exec . nix flake update nyxt-source`. Lisp sources follow upstream's
`ocicl.csv`; `fetch-dependencies.py` verifies OCI manifests and layers, and the
derivation's recursive output hash pins the complete source tree, including Git
dependencies. If the dependency set changes, reset `dependencies.outputHash` to
`lib.fakeHash`, build, and use the reported hash after reviewing the source pins.

Keep the CEF URL and hash aligned with upstream's Linux build scripts. Build and
check page loading, Lisp evaluation, renderer sandboxing and clean shutdown before
activating an updated package. A source update alone does not update Chromium.
