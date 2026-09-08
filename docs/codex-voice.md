# Codex voice on Andromeda and Foundation

Local GPU dictation and spoken Codex replies in one Herdr pane. Whisper small.en
recognizes speech; audio.cpp runs Qwen3-TTS 0.6B with the pinned Samantha
reference from *Her*. Andromeda uses CUDA for TTS and NVIDIA Vulkan for
Whisper on its RTX 5090. Foundation uses Radeon Vulkan for both services.
Foundation was offline, so activation
and live GPU, microphone and playback checks remain pending. The source recording,
transcript, checksums and file-history investigation are in
[`home/config/voice/voices`](../home/config/voice/voices/README.md).

## Use

Run `codex-voice` in the Herdr pane you want to talk to. Normal Codex options
work, including `codex-voice resume SESSION_ID`. This retains the existing
Codex wrapper, provider configuration and approvals. The launcher selects
that exact pane and process. Starting another voice launcher replaces the
selection; other ordinary Codex sessions remain independent.

| Hotkey | Action |
| --- | --- |
| Super+Space | Start recording; press again to stop and transcribe |
| Super+Shift+Space | Send the dictated draft to the selected Codex session |
| Super+R | Read the latest completed reply; press again to stop speaking |

Super is the Windows key. Optional sound cues mark microphone capture. The
transcription appears in the prompt for review before you send it. Recording
stops after three minutes. These shortcuts are configured on Andromeda and
Foundation.

The microphone tray icon is grey when idle, amber while starting or processing,
red once audio samples are arriving, and green when dictation is ready. Open
its menu for elapsed recording time, input level, errors and the same
record/send/cancel/read controls as the hotkeys. A zero input level while
recording means you should check the selected microphone and mute settings.
The tray stays responsive during slow terminal operations and reconnects if
COSMIC's status area restarts.

Capture can start while Codex is working or Herdr briefly reports an unknown
state. Delivery still requires the selected Codex process to be ready. If it
is busy or unreachable before pasting, the transcript is retained in memory:
press Send when Codex is ready. Cancel discards retained text. A lost paste
acknowledgement is reported separately; inspect the selected prompt and send
it there if it arrived. The controller never retries an uncertain paste.

Silence, short recordings, punctuation-only results and Whisper markers such
as `[BLANK_AUDIO]` or `[MUSIC]` are skipped and disarm Send. No Enter is sent
automatically. Failed sound cues and desktop notifications do not abort
dictation. Microphone startup requires samples within five seconds; a stream
that stops producing samples for three seconds is reported as stalled.
Cancellation and stop escalate stuck recorder processes and reap them within
about 1.1 seconds.

Replies speak automatically by default. `codex-voice auto off` switches to
manual playback; `codex-voice auto on` restores automatic playback. This option
lasts until the controller restarts. `codex-voice stop` cancels recording or
playback. `codex-voice status` reports the selected pane and audio state.

Speech skips fenced code and simplifies Markdown. It reads the remaining
prose in full, so ask Codex for concise replies when you want brief audio.
On Andromeda, playback starts after the first chunk is prepared. The next
chunk is synthesized while the current one plays, using one continuous audio
stream. Foundation prepares the complete reply before playback, so its slower
GPU cannot introduce synthesis pauses during speech. Super+R or
`codex-voice stop` cancels preparation as well as playback.
Recording interrupts speech, and a reply that completes during recording
waits for manual playback with Super+R.

Microphone and speakers follow the PipeWire defaults selected in COSMIC sound
settings. At setup on Andromeda these were Razer Kiyo Pro Ultra and Audioengine
2+. Foundation's audio devices still need a live check.

## Session behavior

The wrapper attaches a notification callback to its own Codex launch. A
private token and the first accepted conversation ID filter completed replies.
Inherited subagent notifications are rejected using read-only Codex metadata.
This adapter currently expects `state_5.sqlite` and thread source `cli`; an
unsupported schema or an `exec`-origin conversation fails closed for playback.

Keep one conversation per wrapper launch. After `/new` or switching to another
conversation inside Codex, exit and launch `codex-voice` again, optionally with
`resume SESSION_ID`, to select the new conversation for spoken replies.

Input is pasted through Herdr's bracketed-paste-aware socket API. Newlines do
not submit the draft. Before pasting or sending, the controller checks the
recorded PID, process start time, foreground process and Herdr's agent state.
Working or blocked agents refuse delivery. The send action uses Herdr's own
guarded prompt operation. Avoid pressing Enter manually at the same time as
the send hotkey: Herdr's delayed submit is not atomic with a user's keystroke.

The live pane selection, bound conversation ID and completed-turn identifiers
survive a controller restart if the same Codex process is still running.
Retained dictation, the previous reply and playback
preferences do not survive that restart. Exiting the selected Codex process
clears the selection and stops both model services, releasing their GPU memory.

Speech recognition and synthesis are local. Prompt text still goes to the
model provider configured for Codex.

## Models and services

The default Whisper, Qwen3 and fallback Supertonic models occupy about 2.93 GB in
`~/.local/share/codex-voice/models`. Downloads have pinned revisions, sizes and
SHA-256 checksums. Run `codex-voice-models` after installing on a fresh machine,
or `codex-voice-models --check-only` to verify the local files. Model downloads
are separate from Home Manager activation. The small reference WAV and
transcript are included in the dotfiles and copied into the immutable Nix
store with the configuration. Inference needs no Python ML packages, remote
speech APIs or runtime downloads.

On Foundation, run `hmswitch` from this checkout when the laptop is online,
then `codex-voice-models` and `codex-voice-models --check-only`. Check the Radeon
Vulkan driver and try dictation and spoken playback on the laptop before
considering its setup verified. Terminus and Relay are excluded from this
configuration.

The launcher starts three user services on demand:

- `codex-voice.service`: hotkeys, capture, selected session and playback.
- `codex-voice-stt.service`: resident Whisper on `127.0.0.1:8178`.
- `codex-voice-tts.service`: resident Qwen3 on `127.0.0.1:8179`.

The controller also owns the tray icon, using a pinned Python D-Bus library.
The icon is present while that service runs, including a grey state without a
selected session. No extra tray daemon, model or network service is required.
The services are not enabled at login. Runtime state and temporary recordings live in
the private `$XDG_RUNTIME_DIR/codex-voice` directory. Recordings are deleted
after transcription or cancellation. Buffered speech uses one unnamed temporary
WAV, released after playback, cancellation, synthesis failure or service exit.
Streaming retains only the current and next chunk in memory and feeds PCM to
one PipeWire player. Cancelling terminates playback immediately; an outstanding
synthesis request finishes before its result is discarded. A synthesis failure
stops the stream and reports an error, so a streamed reply can be partially heard.
The speech server disables request-body
logging; Whisper's diagnostic journal can contain recognized text.

Stop all three explicitly with:

```sh
systemctl --user stop codex-voice.service codex-voice-stt.service codex-voice-tts.service
```

Inspect failures with:

```sh
journalctl --user -u codex-voice -u codex-voice-stt -u codex-voice-tts -n 100
```

The models take several seconds to load on first launch. If an immediate first
request fails during startup, wait for startup to finish and retry. Ports 8178
and 8179 must be available.

## Voice choice and measured performance

The model and `default_voice_preset` are set in `home/user/codex-voice.nix`.
The preset references the committed WAV and its exact transcript. Model weights
have fixed Hugging Face revisions, sizes and SHA-256 hashes in
`home/config/voice/voice-model-setup`. Change the managed configuration, build
and run `hmswitch` to change the voice. Do not overwrite a reference beneath a
running server: Qwen3 caches its encoded prompt. Restart TTS after changing it.

Measurements on Andromeda's previous Radeon RX 7600 on 2026-09-07 with both
model services resident:

| Operation | Measured result |
| --- | --- |
| Whisper, upstream 11-second JFK fixture | Accurate transcription in 0.40 s |
| Qwen3 Samantha, first resident request | 6.32 s of audio in 9.93 s |
| Qwen3 Samantha, warm requests | 2.62 to 4.53 s synthesis |
| Qwen3 Samantha, 242-character response | 12.72 s of audio in 6.05 s |
| Whisper resident GPU memory | About 647 MiB VRAM |
| Qwen3 after the prose request | About 2.69 GiB idle VRAM |
| Qwen3 transient prose peak | About 5.95 GiB VRAM and 2.41 GiB GTT |

These measure local inference, excluding Codex response time, hotkey dispatch
and device startup. GPU logs and per-process DRM counters confirmed Vulkan
execution for both services. A cold Qwen3 process took about 12.5 seconds
including model, reference and shader setup. The first reference encoding
takes roughly eight seconds; later requests reused it in about one
millisecond. Temporary synthesis buffers grow with output duration, then
release memory. Four sequential resident requests and the longer response
were checked; the full text was recovered with local Whisper.

Andromeda was switched with `hmswitch` on 2026-09-07. The selected pane and
launcher identity survived activation; all three services ran with zero
restarts. Two requests through the installed TTS configuration produced
complete speech, verified by local Whisper and PipeWire playback. ASR
comparison ignored punctuation and treated "you're" as equivalent to "you
are". The installed reference matched its recorded SHA-256.

Qwen3 1.7B was also tested on the Radeon, but used about 1.2 GiB more peak GPU
memory for the short audition. The 0.6B model remains the accepted voice.
Replies use chunks of at most 260 characters, apart
from individual longer words, with a 256-token synthesis limit per chunk.
Inference remains sequential in both playback modes. On Andromeda, one worker
prepares the next chunk while the current PCM is written to the playback pipe;
pipe backpressure bounds the amount of queued audio. In buffered mode, all PCM
samples are joined before playback. The `playback_mode` setting in
`home/user/codex-voice.nix` selects `streaming` for Andromeda and `buffered` for
Foundation. Neither mode loads another model onto the GPU.

The initial RTX 5090 verification on 2026-09-08 used NVIDIA 595.99.02 and both
speech services on NVIDIA Vulkan. For the same 237-character sample, warmed-up
0.6B inference generated 14.32 seconds of audio in 1.873 seconds; 1.7B generated
12.00 seconds in 1.754 seconds. First requests took 19.455 and 22.002 seconds
respectively, including voice-reference and shader setup. These are individual
measurements, not a general speed ranking. Both recovered the full text through
Whisper, allowing contractions; one 0.6B transcription also contained a `[MUSIC]`
tag. Listening is still needed to judge voice similarity and prosody.

A live two-chunk 0.6B playback check started the PipeWire stream after 2.518
seconds. The second chunk was ready 13.883 seconds before the first chunk's
audio ended. Playback completed normally in 23.03 seconds for 20.48 seconds of
audio, including the initial preparation time.

Andromeda also runs a local Qwen coding model. Its TTS runtime uses CUDA 12.9
on Blackwell and a patch to the pinned audio.cpp decoder. When an utterance
needs a different decoder shape, the patch releases the previous graph and
its CUDA graph cache before allocating the replacement. This prevents the
temporary overlap that caused allocation failures while Qwen was resident.
Matching decoder graphs remain cached. Foundation retains Vulkan without
this CUDA-specific patch. See [local Qwen](local-llm.md) for the shared memory
budget and measured context configuration.

The larger model is not a universal English voice-cloning improvement:
[upstream evaluations](https://huggingface.co/Qwen/Qwen3-TTS-12Hz-1.7B-Base#evaluation)
are mixed across benchmarks. Those results use BF16 models, while these local
models use Q8 weights. The 0.6B preset remains the default pending a preferred
audition.

Supertonic F1 is retained as a fast fallback model, with about 0.3 to 0.6
seconds of synthesis for short replies. Earlier PocketTTS and dots.tts
Samantha auditions were rejected before the GitHub reference was recovered.

Experimental Qwen3 1.7B, dots.tts and PocketTTS assets are also available.
They are not loaded by the services. The optional
`codex-voice-models --experimental --check-only` verifies them. Audition files
are in `~/.local/share/codex-voice/voices`, including
`qwen3-0.6b-samantha-preview.wav`, `qwen3-1.7b-samantha-preview.wav` and
`qwen3-0.6b-samantha-prose-preview.wav`.

## Development checks

```sh
direnv exec . python3 -m unittest discover -s tests -p test_codex_voice.py -v
direnv exec . nix build \
  '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . nix build \
  '.#homeConfigurations."william@foundation".activationPackage' --no-link
direnv exec . nix flake check
direnv exec . hmswitch
```

Run all recording and tray regressions with the deployed Python environment:

```sh
# The interpreter used by the installed codex-voice launcher includes dbus-next.
direnv exec . nix shell --impure --expr \
  'let f = builtins.getFlake (toString ./.); in f.nixosConfigurations.andromeda.pkgs.python3.withPackages (p: [ p.dbus-next ])' \
  -c python3 -m unittest discover -s tests -p 'test_*voice*.py' -v
```

Behavioral tests cover literal input, guarded submission, stale processes,
replaced sessions, subagent filtering, duplicate replies, silence, cancellation
and restart recovery, including the bound conversation and stale dictation
after manual submission. Speech tests check complete buffering, sample order,
one playback, and cleanup after cancellation, failed requests or invalid WAVs.
Streaming tests check early playback, synthesis during playback, PCM order,
cancellation of prefetched speech, failed requests and pipe cleanup.
Live verification also requires a real Codex completion and a microphone trial,
since mocked tests cannot establish desktop hotkey or physical audio behavior.
Capture tests use real subprocess fixtures for startup timeout, cancellation,
stalls, disconnects, partial PCM reads and file-write failures. Tray integration
uses a private D-Bus session to check registration, icon changes, menu actions,
watcher replacement and responsiveness during blocked controller status reads.
