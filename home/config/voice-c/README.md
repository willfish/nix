# Native voice frontend

The C frontend provides the voice controller and CLI, GTK status pill, Fuzzel
menu and PersonaPlex conversation launcher. Home Manager installs them through
`home/user/voice-c-package.nix`. The `pi-voice` and `qwen-pi-voice` wrappers retain
the existing configuration and credential loading, then execute the native CLI.

The Pi extension still uses the local JSON socket protocol. Model downloads and
the PersonaPlex inference server remain separate Python-based tools; they are
not part of the frontend migration.

## Build and test

The Linux flake check builds and tests the same package that Home Manager installs,
with pinned dependencies:

```sh
direnv exec . nix build --no-link .#checks.x86_64-linux.voice-c
```

For incremental development, use the C shell from the repository root:

```sh
direnv exec . nix-shell home/config/voice-c/shell.nix --run '
  umask 077
  meson setup /tmp/voice-c-build home/config/voice-c -Dbuildtype=debugoptimized
  meson test -C /tmp/voice-c-build --print-errorlogs
'
```

Use a fresh build directory for each checkout. To repeat tests in that checkout,
run just the `meson test` command in the same shell. The suite uses fake
subprocesses and loopback servers; it does not require a microphone, a live speech
backend or a desktop session. Unit tests are C; daemon and recording journeys are
TypeScript, run with Node's built-in TypeScript support. Capture fixtures also use
Node rather than Python.

Run the model-tool and packaging assertions from the repository root with:

```sh
direnv exec . node --test tests/voice-model-tools.test.ts tests/voice-native-wiring.test.ts
```

Those TypeScript tests invoke retained Python production APIs through a small
language adapter. Test cases and assertions stay in TypeScript; the adapter only
sets up API inputs and serializes results. It does not access real credentials.

Meson logs include the inherited environment;
keep development build directories private and never commit or publish those logs.

For address and undefined-behavior checks, configure a separate build with
`-Db_sanitize=address,undefined`. Fontconfig retains process-global font patterns;
if these are reported, use a targeted LeakSanitizer suppression for
`libfontconfig.so`, not a blanket disable of leak detection.

ThreadSanitizer needs instrumented GLib, Pango and Fontconfig to validate the
painting path. A suppression matching `pill_paint` excludes that path, not just
external allocations, and must not be described as a race-free rendering check.
Allow a larger Meson timeout for instrumented runs (`--timeout-multiplier 3`).

## Text contracts

Text inputs are UTF-8. Summary and chunk limits count Unicode code points rather
than bytes. Invalid UTF-8 is rejected; transcript and summary output buffers never
receive a silently truncated result. The headers document error returns and
caller ownership. Concurrent first calls may safely initialize text patterns.

Passing the offline suite is not a deployment check. Verify the installed daemon,
CLI and GTK process after activation, along with capture, playback and desktop
interaction. A cancelled HTTP request can outlive the controller operation that
started it: keep callback contexts and dependencies alive until workers drain.
Injected schedulers and executors must be drained before freeing their manager or
label cache, as described in the headers. Do not restart the controller while
unrecovered dictation or retry audio is still in memory.
