# PersonaPlex tools

`personaplex-models --data-dir DIR [--check-only]` verifies and installs the five
pinned assets outside the Nix store. The Andromeda wrapper supplies the data path
and a private umask. Cached assets require matching size and SHA-256; check-only
creates the private root but does not read credentials, download or extract.

Downloads require an existing `HF_TOKEN` or Hugging Face cache token and accepted
model terms. Credentials stay in memory, are never included in diagnostics, and
are removed permanently when a redirect changes authority. Redirects must remain
HTTPS. TLS verifies peer and hostname, honoring `SSL_CERT_FILE`; the 60-second
connection/idle timeout is not a whole-download deadline. Downloads use adjacent
private temporary files, verify pins before rename and remove failed temporaries.

Archive names/types are validated before extraction. The per-entry data filter
rejects links, special files and paths resolving outside the root, while retaining
safe internal aliases, filtered file modes and timestamps. This preserves the
upstream Python data-filter behavior without a Python installer.

`personaplex-patch SOURCE BROWSER-GUARD` applies the pinned server guards and
browser cleanup injection during packaging, failing closed when anchors change.
`src/server-guard.inc` is patch data for the excluded third-party moshi runtime,
not an executable repository Python utility. The runtime remains third-party
Python; its host, GPU-memory and access safeguards are unchanged.

## Manual fixtures

Fixtures are off by default, never installed, and have no automatic test wiring.
Only the fixture executable accepts custom model pins, endpoints and timeouts.

```sh
direnv exec . nix develop .#personaplex-tools -c \
  meson setup /tmp/personaplex-tools-build home/config/personaplex-tools -Dfixtures=true
direnv exec . nix develop .#personaplex-tools -c ninja -C /tmp/personaplex-tools-build
PERSONAPLEX_FIXTURE=/tmp/personaplex-tools-build/personaplex-fixture \
PERSONAPLEX_MODELS_BIN=/tmp/personaplex-tools-build/personaplex-models \
PERSONAPLEX_PATCH_BIN=/tmp/personaplex-tools-build/personaplex-patch \
  direnv exec . nix shell nixpkgs#openssl -c \
  node --experimental-strip-types --test home/config/personaplex-tools/tests/tools.test.ts
```

Fixtures use disposable TLS endpoints, small generated archives and fake tokens.
They do not accept model terms, use live credentials, fetch gated weights or run
GPU inference. For ASan/UBSan, add `-Db_sanitize=address,undefined -Db_lundef=false`
to a fresh Meson build directory and point the same driver at its executables.
