# Local voice API

`pi-voice-api --config FILE` serves the local Deepgram REST subset on
`127.0.0.1:8180`. Configuration and caller contracts are documented in
[`docs/voice.md`](../../../docs/voice.md). Whisper and audio.cpp remain separate
processes; this binary handles HTTP translation and engine lifecycle only.

Hyper handles HTTP framing. Requests and responses have bounded sizes and
timeouts, inference is serialized, and signal shutdown drains requests before
stopping engines. Both inference and health URLs must use numeric IPv4 loopback
HTTP. Upstream connections ignore proxy environment variables, never follow
redirects, and never forward client credentials. Request data and engine output
are not logged.

Build and run the unit and fixture tests with:

```sh
direnv exec . nix build -L .#checks.x86_64-linux.voice-api
```

The package check runs `tests/voice-api.test.ts` against the just-built binary
using stub HTTP engines and a fake systemctl. It does not load models or touch
live services. For local Cargo builds, set `PI_VOICE_API_TEST_BIN` to the candidate
executable before running that Node test. Without an override, the test builds
the current checkout's Nix check, never an installed profile binary.

Cargo dependencies are locked. TLS and a general-purpose web framework are not
needed for this loopback-only service.
