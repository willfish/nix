# Pinned voice models

`voice-model-setup` installs the local speech assets pinned in `src/assets.c`.
The public `pi-voice-models` wrapper supplies the host name and remains enabled
only where local speech engines are supported. `--stt-only`, `--experimental`,
`--host`, `--data-dir` and `--check-only` retain their existing meanings.

The default directory is `${XDG_DATA_HOME}/pi-voice`, or
`$HOME/.local/share/pi-voice` when XDG_DATA_HOME is unset. An explicitly empty
XDG_DATA_HOME keeps the relative `pi-voice` directory.

Every asset requires both the pinned byte count and SHA-256. Check-only does not
create directories or download. Downloads stream into unique mode-0600
`.partial` files beside their destination, flush and fsync before an atomic
rename, and remove failed temporaries. Existing valid files keep their mode.
During downloads, HTTP/network and filesystem failures get three attempts with
one- and two-second backoff. Size/hash failures and truncated chunk framing are not retried. Pinned
bytes remain authoritative if an otherwise complete response overstates its
Content-Length.

The 30-second connection/idle timeout does not cap a progressing model download.
TLS checks the CA and hostname; a declared SSL_CERT_FILE supplies its CA bundle.
The downloader preserves the `pi-voice-model-setup` User-Agent, identity
encoding and closed HTTP/1.1 connections. It does not read model credentials.

## Manual fixtures

Fixtures stay in this program root and never run from a package build, flake
check or hook. From the repository root with its direnv environment active:

```sh
nix develop .#voice-models --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/voice-models-checks home/config/voice-models \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#voice-models --command meson compile -C /tmp/voice-models-checks
nix shell nixpkgs#openssl -c /tmp/voice-models-checks/model-checks
```

`VOICE_MODELS_BIN` and `VOICE_MODELS_FIXTURE` still select those executables;
otherwise the checks use the siblings from the same build directory. OpenSSL is
required only for the TLS trust case. Configure once without `-Dfixtures=true`
to confirm the package build does not create the check executables.

For ASan/UBSan, use a separate build directory with
`-Db_sanitize=address,undefined -Db_lundef=false`, then point both executable
variables at that build. The fixture accepts small local HTTP assets without
changing the production command's pinned manifest. Its short test deadlines and
backoff do not change the production policy.
