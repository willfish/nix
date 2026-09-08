# Local agent voice on Andromeda and Foundation

One local recorder, tray and pair of speech engines serve Codex, Grok and Pi.
Whisper small.en recognizes speech with Silero VAD; audio.cpp runs Qwen3-TTS
0.6B with the pinned Samantha reference. Andromeda uses CUDA for TTS and
NVIDIA Vulkan for Whisper. Foundation uses Radeon Vulkan; its live microphone
and playback checks remain pending. macOS needs separate platform adapters.

## Launch and select a session

Run a voice launcher inside the Herdr pane you want to use:

| Launcher | Coding session |
| --- | --- |
| `codex-voice` | Existing Codex wrapper and provider configuration |
| `grok-voice` | Existing Grok wrapper with local, in-process execution |
| `pi-voice` | Normal Pi profile, providers and MCPs |
| `qwen-pi-voice` | Existing local Qwen Pi profile, tuning and MCPs |

Pass normal interactive options, for example `codex-voice resume SESSION_ID`
or `pi-voice --continue`. Use `--` before options that conflict with a voice
control command. Noninteractive print/RPC/agent modes are excluded. Grok uses
`--no-leader` so its hooks inherit this launcher's token without a separate
daemon. Ordinary launches continue to work independently of voice.

Each voice launcher registers its own process, pane and conversation. The
latest launcher becomes selected. The Agent Voice tray lists other registered
sessions so you can select one explicitly. Keyboard focus never changes voice
selection. Only the selected session receives dictation or automatic playback.
Switching sessions preserves their retained text in memory and cancels current
recording/playback. Exiting one launcher cannot stop engines another registered
launcher is using; the last exit releases both model services.

| Hotkey | Action |
| --- | --- |
| Super+Space | Stop an active recording; otherwise send a prepared voice prompt, or start recording if none is prepared |
| Super+Shift+Space | Explicitly send the dictated draft |
| Super+R | Read the latest completed reply; press again to stop speaking |

Super is the Windows key. Transcription is staged for review; recording never
automatically presses Enter. Capture starts only after microphone samples
arrive and stops after three minutes. Silence, punctuation-only output and
non-speech markers are skipped. Silero VAD adds speech detection beyond the
initial quiet-audio gate, while a small vocabulary prompt helps with names
such as Herdr, Qwen, NixOS and the configured hosts.

The usual flow uses Super+Space three times: record, stop and transcribe, then
send. Wait for the green ready state before the third press. Retained dictation
is delivered and sent when the selected agent is ready; a busy agent leaves
those words retained. The hotkey applies across all four launchers and never
broadcasts to other registered sessions, including multiple sessions of the
same harness.

This decision tracks prompts prepared by voice, including subsequent edits.
Codex and Grok do not expose authoritative editor contents, so entirely
hand-typed prompts do not trigger automatic submission. Clearing their editor
manually does not clear voice's tracked draft. Use the tray's Start recording
or `codex-voice record` to add more speech instead of sending a prepared prompt.

## Tray and recovery

The tray is grey when idle, red while recording, amber during startup or
processing, and green when dictation is ready. Its menu shows the selected
harness/conversation, actual microphone, mute/clipping warnings and speech
model readiness. Andromeda prefers the Razer Kiyo Pro Ultra's stable device
name, with a visible fallback to PipeWire's default if absent. Foundation uses
the default microphone. Speakers follow the PipeWire default.

Recording works while the selected agent is busy. Valid text waits in memory
until it can be safely delivered. A new recording appends to retained text by
default. The menu also offers Replace; replacement happens only after valid
new speech, so silence or a failed start preserves previous words. Discard
removes retained text and retry audio. Cancel does not remove text already
pasted into a terminal editor; that prompt remains available for review.

A failed transcription retains one private WAV for up to two minutes after
its first failure. Retry uses that recording and its original deadline.
Cancel, discard, changing selected sessions or restarting the controller
removes it. A successful transcription deletes the WAV. Model startup has a
bounded readiness wait with loading/error feedback, so the first recording
can wait for Whisper rather than failing immediately.

Cancel signals immediately, independently of slow terminal operations. Once
input has reached an editor it cannot safely be recalled automatically.
Uncertain paste/submission acknowledgements disarm retries; inspect the prompt
before sending manually. A cancelled HTTP caller returns promptly, but an
already submitted model request may finish in the background. Requests remain
serialized until that happens to avoid overlapping GPU allocations.

## Conversation binding and adapters

After `/new` or changing conversations, choose Rebind in the tray (or run
`codex-voice rebind`). Pi/Grok report their new conversation as a candidate;
voice keeps the old binding until you explicitly rebind. For Codex, Rebind
can wait for the first completed reply from the new conversation. Delayed
callbacks from the previous conversation are excluded after rebinding.

Codex uses its per-launch notification callback and Herdr's guarded paste and
submit operations. The callback accepts only root CLI threads, checked through
read-only `state_5.sqlite` metadata. Unsupported schemas fail closed for spoken
replies. Grok's conditional command hooks report root-session lifecycle events;
provisional Stop replies wait for Herdr's ready state before playback. Grok
creates its conversation lazily, so the first dictation can be staged before
its initial session event arrives.

Pi explicitly loads the voice extension, including in `qwen-pi` where automatic
extension discovery is disabled. It uses native editor APIs for staging and
single-use submission, preserving existing typed text. Super+Space explicitly
submits the current edited voice draft. The separate Send action keeps its
unchanged-editor guard; after that guard rejects edits, submit in Pi or record
again. Spoken replies use `agent_settled`, excluding
reasoning, aborted output and intermediate tool turns. Adapter sockets are
private and check launcher token, process and conversation identity.

Registration metadata survives a controller restart while the processes live.
Retained dictation, prior replies, WAV retries and playback preferences do not.
The registry never persists prompt or reply contents.

## Speech and services

Replies speak automatically. Use `codex-voice auto off` for manual playback,
`auto on` to restore it, `stop` to cancel and `status` to inspect state. All four
launchers accept the same controls: `interact`, `record`, `send`, `read`, `stop`, `status`,
`retry`, `rebind`, `discard`, `append` and `replace`.

Speech skips fenced code and simplifies Markdown. It prefers sentence and
clause boundaries with a shorter first phrase and a 260-character maximum.
Andromeda synthesizes one chunk ahead during playback through one continuous
PipeWire stream. Foundation prepares the entire reply before playback. These
are complete-reply playback modes, not live reading of unfinished model text.
Recording interrupts speech; a reply completing during recording waits for
manual playback.

The existing service names remain for compatibility:

- `codex-voice.service`: shared controller, hotkeys and tray.
- `codex-voice-stt.service`: Whisper at `127.0.0.1:8178`.
- `codex-voice-tts.service`: Qwen3 TTS at `127.0.0.1:8179`.

They start on demand, not at login. Ports must be available. The default
models occupy about 2.93 GB in `~/.local/share/codex-voice/models`. Run
`codex-voice-models` on a fresh host, and `codex-voice-models --check-only` to
verify sizes and hashes. Silero VAD is also fetched with a fixed hash by Nix,
so enabling it requires no extra setup on Foundation. Inference uses local
files without cloud speech APIs or runtime Python package downloads. Prompt
text still goes to the provider selected by the coding harness.

The voice reference, transcript and provenance are in
[`home/config/voice/voices`](../home/config/voice/voices/README.md). Runtime
state, private adapter sockets and temporary recordings use
`$XDG_RUNTIME_DIR/codex-voice`. Whisper's diagnostic journal can contain
recognized text; the TTS server disables request-body logging.

Stop the services explicitly with:

```sh
systemctl --user stop codex-voice.service codex-voice-stt.service codex-voice-tts.service
```

Inspect failures with:

```sh
journalctl --user -u codex-voice -u codex-voice-stt -u codex-voice-tts -n 100
```

On Foundation, pull the configuration, run `hmswitch`, then
`codex-voice-models` and verify the microphone, Radeon Vulkan and playback.
Terminus and Relay are excluded. The macOS/Relay browser setup remains text-only.

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
direnv exec . node --test tests/pi-voice.test.mjs
direnv exec . nix build \
  '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . nix build \
  '.#homeConfigurations."william@foundation".activationPackage' --no-link
direnv exec . nix flake check
direnv exec . hmswitch
```

Run all controller, recording, adapter and tray regressions with dbus-next:

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
Recovery tests cover a blocked terminal during Cancel, retained dictation across
sessions and retries, absolute WAV expiry, late acknowledgements and rebinding.
Primary-hotkey tests cover the record/transcribe/send flow, busy agents,
in-progress transcription, cancellation and submission to one selected session.
Pi tests exercise native staging, changed-editor rejection, final reply filtering
and socket framing. Grok tests cover its installed hook schema and root events.
Isolated installed-harness checks verify Pi extension loading and a real Grok
TUI completion against a loopback mock. Physical microphone and listening trials
are still needed to assess recognition and speech quality.
Capture tests use real subprocess fixtures for startup timeout, cancellation,
stalls, disconnects, partial PCM reads and file-write failures. Tray integration
uses a private D-Bus session to check registration, icon changes, menu actions,
watcher replacement and responsiveness during blocked controller status reads.
