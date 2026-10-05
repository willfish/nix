# Voice C library

This is an incremental port of the Python voice modules in `../voice`. It builds
`libpivoice` and an offline test driver. It does not install a controller, tray,
menu or GTK application, and Home Manager still runs the Python implementation.
The pill module provides a view model and Cairo painting, not a standalone window.

## Build and test

The Linux flake check builds and runs the full C suite with pinned dependencies:

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
backend or a desktop session. Meson logs include the inherited environment;
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

Passing the offline suite is not a deployment check. Controller integration and
live capture, playback and desktop interaction still need verification before
switching the installed implementation to C. Released engine leases remain owned
by their manager until destruction; long-running controller integration needs an
explicit reclamation policy. Injected schedulers and executors must be drained
before freeing their manager or label cache, as described in the headers.
