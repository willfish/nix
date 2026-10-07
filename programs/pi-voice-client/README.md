# Pi voice client

The C client provides the voice controller and CLI, GTK status pill, Fuzzel
menu, recording/playback and Pi/Herdr session integration. Its package definition
is `default.nix`; Home Manager connects it through `home/user/voice.nix`.
The public `pi-voice` and `qwen-pi-voice` wrappers retain configuration and
credential loading before executing the native CLI.

The Pi extension uses the client's local JSON socket protocol. Speech requests
can go to the [local voice API](../voice-api) or a configured remote provider;
the client is not tied to one backend. Model setup and inference engines remain
separate programs.

## Build and test

Home Manager builds the runtime without test targets or automatic test phases.
For manual migration verification, use the C shell from the repository root:

```sh
direnv exec . nix-shell programs/pi-voice-client/shell.nix --run '
  umask 077
  meson setup /tmp/voice-c-build programs/pi-voice-client -Dbuildtype=debugoptimized -Dfixtures=true
  meson compile -C /tmp/voice-c-build
  /tmp/voice-c-build/voice-tests
  /tmp/voice-c-build/voice-json-allocation
  /tmp/voice-c-build/voice-behavior /tmp/voice-c-build/pi-voice-c
'
```

Use a fresh build directory for each checkout. These program-local fixtures use
fake subprocesses and loopback servers, not a microphone, live speech backend,
desktop session or real credentials. They are not registered as flake, package or
commit-hook checks.

Meson logs include the inherited environment;
keep development build directories private and never commit or publish those logs.

For address and undefined-behavior checks, configure a separate build with
`-Db_sanitize=address,undefined`. Fontconfig retains process-global font patterns;
if these are reported, use a targeted LeakSanitizer suppression for
`libfontconfig.so`, not a blanket disable of leak detection.

ThreadSanitizer needs instrumented GLib, Pango and Fontconfig to validate the
painting path. A suppression matching `pill_paint` excludes that path, not just
external allocations, and must not be described as a race-free rendering check.
Allow additional time for instrumented manual runs.

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
