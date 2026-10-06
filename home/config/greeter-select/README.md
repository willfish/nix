# Greeter selection

`greeter-select MANIFEST` publishes the NixOS login theme's immutable asset link.
The root-generated manifest supplies `selectionDir`, `selectionName`, `runtimeDir`,
`themes` and `fallback`. It does not accept desktop-supplied filesystem paths.

- Open the selection parent with `O_DIRECTORY|O_NOFOLLOW`, and its leaf relative
  to that descriptor with `O_NOFOLLOW|O_NONBLOCK`. Require a regular file and read
  at most 65 bytes. Accept at most 64 bytes including an optional final LF, using
  lowercase ASCII alphanumeric segments separated by single hyphens.
- Missing, unusable and unknown selections use the declared allowlisted fallback.
  A malformed selected entry fails rather than silently selecting another theme.
  Asset strings must start `/nix/store/`, contain no LF/NUL and have no exact `..`
  path component. Retain their spelling; do not resolve them or require existence.
- `lstat` the runtime directory: real directory, effective-UID owner and no
  group/world write. Create a private mode-0700 `.theme-` directory subject to
  umask, create its temporary `omarchy` link, then atomically replace the runtime
  leaf. Failed publication retains the old leaf and removes the owned temporary.
  Cleanup failure after a successful rename does not roll back the new link.
- Retain strict UTF-8 manifest loading, last-duplicate-key behavior, Python JSON
  constant spellings and escaped-surrogate semantics. Map filesystem
  surrogateescape code points back to bytes; other lone surrogates are unusable
  filesystem paths. Never truncate NUL-bearing paths.

The system module retains service ordering, tmpfiles, filesystem confinement,
selection-file refresh, password authentication and `QML_DISABLE_DISK_CACHE=1`.
Home Manager activation alone does not update this system service. Build and
activate the affected NixOS host before relying on the deployed selector; do not
restart the display manager just to verify this helper.

## Manual fixtures

With GLib, yyjson, Meson, Ninja and pkg-config supplied by an ephemeral Nix shell
from the direnv checkout:

```sh
meson setup /tmp/greeter-build home/config/greeter-select \
  -Dfixtures=true -Dbuildtype=debugoptimized
meson compile -C /tmp/greeter-build
GREETER_SELECT_BIN=/tmp/greeter-build/greeter-select \
GREETER_SELECT_FIXTURE=/tmp/greeter-build/greeter-fixture \
GREETER_SELECT_FAILURES=/tmp/greeter-build/greeter-failures.so \
  node --experimental-strip-types --test \
    home/config/greeter-select/tests/greeter.test.ts
```

The fixtures are optional, noninstalled and unregistered. They use disposable
manifests, selections and runtime directories with intentionally dangling store
links. They never edit live login assets or restart a login session. Optional
whole-command legacy parity uses `GREETER_SELECT_LEGACY` and
`GREETER_SELECT_PYTHON`. `GREETER_SELECT_ASAN_RT` puts the compiler's ASan runtime
before the failure interposition library. Production recognizes no fixture
controls.
