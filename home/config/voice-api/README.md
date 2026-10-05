# Local voice API

`pi-voice-api --config FILE` serves the local Deepgram REST subset on
`127.0.0.1:8180`. Home Manager supplies its configuration through
[`home/user/voice.nix`](../../user/voice.nix). Whisper and audio.cpp remain separate
processes; this binary handles HTTP translation and engine lifecycle only.

Hyper handles HTTP framing. Requests and responses have bounded sizes and
timeouts, inference is serialized, and signal shutdown drains requests before
stopping engines. Both inference and health URLs must use numeric IPv4 loopback
HTTP. Upstream connections ignore proxy environment variables, never follow
redirects, and never forward client credentials. Request data and engine output
are not logged.

Build with `direnv exec . nix build .#voice-api`. Cargo tests are program-local
and manual; package builds and flake hooks do not run them. Stub engines and
process adapters do not establish live model or desktop operation.

Cargo dependencies are locked. TLS and a general-purpose web framework are not
needed for this loopback-only service.
