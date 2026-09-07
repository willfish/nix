# Codex voice on Andromeda and Foundation

Local GPU dictation and spoken Codex replies in one Herdr pane. Whisper small.en
recognizes speech; audio.cpp runs Qwen3-TTS 0.6B with the pinned Samantha
reference from *Her*. Both use Vulkan on the configured AMD hosts. Andromeda's Radeon RX 7600
has been tested; Foundation is configured but was offline, so activation and
live GPU, microphone and playback checks remain pending. The source recording,
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

Super is the Windows key. Start and stop cues mark microphone capture. The
transcription appears in the prompt for review before you send it. Recording
stops after three minutes. These shortcuts are configured on Andromeda and
Foundation.

Replies speak automatically by default. `codex-voice auto off` switches to
manual playback; `codex-voice auto on` restores automatic playback. This option
lasts until the controller restarts. `codex-voice stop` cancels recording or
playback. `codex-voice status` reports the selected pane and audio state.

Speech skips fenced code and simplifies Markdown. It reads the remaining
prose in full, so ask Codex for concise replies when you want brief audio.
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
Pending dictation, the previous reply and playback
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

They are not enabled at login. Runtime state and temporary recordings live in
the private `$XDG_RUNTIME_DIR/codex-voice` directory. Recordings are deleted
after transcription or cancellation. The speech server disables request-body
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

Measurements on Andromeda on 2026-09-07 with both model services resident:

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

Qwen3 1.7B was also tested, but used about 1.2 GiB more peak GPU memory for the
short audition. The 0.6B model is the default to leave more room for the desktop
and recognition engine. Replies use chunks of at most 260 characters, apart
from individual longer words, with a 256-token synthesis limit per chunk.

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
direnv exec . nix build '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . nix build '.#homeConfigurations."william@foundation".activationPackage' --no-link
direnv exec . nix flake check
direnv exec . hmswitch
```

Behavioral tests cover literal input, guarded submission, stale processes,
replaced sessions, subagent filtering, duplicate replies, silence, cancellation
and restart recovery, including the bound conversation and stale dictation
after manual submission. Live verification also requires a real Codex completion
and a microphone trial, since mocked tests cannot establish desktop hotkey or
physical audio behavior.
