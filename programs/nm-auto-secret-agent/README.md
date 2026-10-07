# Noninteractive NetworkManager secret agent

Rust service for resupplying a stored system-connection Wi-Fi PSK, including
`REQUEST_NEW` after a handshake timeout. The existing Bash reconnect watcher is
separate and unchanged. Home Manager enables both only on private graphical
Linux profiles.

Rust owns lookup policy, fixed subprocess arguments, text decoding, diagnostics
and shutdown state. A small compiled C bridge implements the `NMSecretAgentOld`
vtable, serialization and GLib lifecycle. Keep this boundary on libnm: it owns
registration, manager-loss recovery and connection validation. It also avoids
PyGObject callback marshalling, where the former extra user-data argument could
prevent successful or failed requests from completing.

## Contract and security

- Keep identifier `org.dotfiles.nm-auto-secret-agent`, capabilities zero, explicit
  enable/register and exit status based on successful initial registration.
- Prefer connection UUID, then ID. Only supply `802-11-wireless-security.psk`.
  Flags do not force an interactive prompt or change the stored-secret lookup.
- Invoke the build-pinned `nmcli -s -g 802-11-wireless-security.psk connection show
  KEY` as literal arguments, without a shell. There is no runtime command override.
- Preserve universal-newline decoding, LF-only trailing stripping and unbounded
  pipe draining. Empty/failed/invalid output returns `NoSecrets`. Reject embedded
  NUL instead of silently truncating a credential. Never echo raw output or
  credential-bearing decoding exceptions. Save/delete requests remain no-ops.
- SIGINT/SIGTERM cancels an in-flight request through libnm, terminates and reaps
  a blocked lookup child, destroys the agent and flushes its unregister message.
  Cancellation is not an invitation to prompt or reconnect Wi-Fi.
- Preserve the system-bus authorization boundary. Libnm authenticates the
  NetworkManager name owner's UID before registration; system D-Bus policy also
  restricts calls to the secret-agent interface. Do not claim libnm independently
  compares every method sender with that owner. Do not remove the bus policy or
  redirect production to an untrusted bus.

The upstream `LIBNM_USE_SESSION_BUS` switch is retained for isolated libnm tests.
It is not a production authorization substitute. Fixtures must set both bus
addresses to their disposable daemon; no fixture connects to the real system bus.

## Manual verification

Use a direnv checkout and an ephemeral Nix shell containing Rust, Cargo, rustfmt,
Clippy, pkg-config, a C compiler, GLib, NetworkManager development libraries and
D-Bus. There are no third-party Rust crates.

```sh
export CARGO_TARGET_DIR=/tmp/nm-agent-target
cargo test --manifest-path programs/nm-auto-secret-agent/Cargo.toml --all-features
cargo clippy --manifest-path programs/nm-auto-secret-agent/Cargo.toml \
  --all-targets --all-features -- -D warnings
```

The `fixtures` feature defaults off; tests and fixture executables are manual
and uninstalled. `cargo test --all-features` runs the lookup tests and the D-Bus
journey. That journey compiles `tests/peer.c` with `cc -std=c17 -Wall -Wextra
-Werror` and `pkg-config --cflags --libs gio-2.0` unless `NM_AGENT_PEER` is set,
then spawns that peer and the fixture binary. Set `NM_AGENT_FIXTURE` only to
override the Cargo-built fixture. The peer exercises the actual libnm D-Bus
interface with disposable connection data and a fake `nmcli`. This does not
establish live Wi-Fi recovery. Optional `NM_AGENT_WIRING` and
`NM_AGENT_PUBLIC_WIRING` JSON inputs verify evaluated host service boundaries
without contacting those hosts.

For the packaged binary, build `tests/route.c` as a shared library with `-fPIC
-shared -ldl`, then set `NM_AGENT_BIN` and `NM_AGENT_ROUTE`. Routing interposition
exists only in that noninstalled fixture library. Optional comparison uses
`NM_AGENT_LEGACY`, `NM_AGENT_PYTHON` and the matching GI typelib path. The retained
Python reference needs only its obsolete explicit callback user-data arguments
removed for current PyGObject; do not present that as unmodified service parity.

For C-bridge ASan/UBSan checks, build the fixture feature in a separate target
directory with `NM_AGENT_C_SANITIZE=1` and rerun the bus fixtures. This instruments
the owned C bridge, not Rust or external libnm. Use distinct sanitizer error exit
codes so expected application failures cannot hide a sanitizer failure.
