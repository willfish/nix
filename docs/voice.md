# Local agent voice on Andromeda

One local recorder, status pill and pair of speech engines serve Pi and Qwen Pi.
The control command is `pi-voice`; units are `pi-voice`, `pi-voice-stt` and
`pi-voice-tts`. Andromeda uses Whisper large-v3-turbo Q5 on NVIDIA Vulkan; Foundation keeps
Whisper small.en on the Radeon iGPU. Pause detection stays in the local
recorder. The recogniser does not run a second VAD pass, because short slices
were being dropped. audio.cpp
runs Qwen3-TTS 0.6B with two pinned Samantha references. Andromeda uses CUDA
for TTS. Foundation has no TTS. macOS needs separate platform adapters.

## Launch and select a session

On Andromeda, and for dictation on Foundation, interactive `pi` and `qwen-pi`
sessions inside Herdr attach automatically. Team panes do not register and cannot
be selected. The lightweight controller starts at login. Speech models
load on use: Whisper for dictation and TTS for playback. Print, RPC and
noninteractive sessions do not attach. After installing this configuration, use
`/reload` once in an existing standard Pi session. Restart an existing Qwen
session so its explicit extension arguments take effect.

The compatibility launchers remain available:

| Launcher | Coding session |
| --- | --- |
| `pi-voice` | Normal Pi profile, providers and MCPs |
| `qwen-pi-voice` | Existing local Qwen Pi profile, tuning and MCPs |

Pass normal interactive options, for example `pi-voice resume SESSION_ID`
or `pi-voice --continue`. Use `--` before options that conflict with a voice
control command. Noninteractive print/RPC/agent modes are excluded.

Each attached session registers its process, pane and conversation. The first
ready non-team Pi session is selected automatically only when selection has
not yet been initialized. Selection stays sticky: new Pi sessions and keyboard
focus never steal it. After the selected session disappears, another session
is not silently chosen. Select a destination manually with **Super+Shift+V**.

Labels identify the harness, workspace, tab and a renamed pane. The terminal title is not used as the pane name.
Only the selected session receives dictation. Team panes are not registered,
so the selector lists ordinary Pi and Qwen Pi sessions only. Pi and Qwen Pi
share the same selector.

Switching sessions cancels recording/playback and preserves retained text in
memory. Exiting one session cannot stop engines another registered session is
using. After the last session leaves, engines stop once active work has drained.

| Hotkey | Action |
| --- | --- |
| Super+Space | Show the top dictation pill, then stop recording, send prepared dictation, or start recording into the selected Pi session |
| Super+Shift+Space | Explicitly send the dictated draft |
| Super+R | Read the latest completed summary; press again to stop speaking |
| Super+Shift+V | Open the keyboard voice/session/action picker |

Super is the Windows key. While recording, a native pill at the top of the
focused monitor shows a capture light, measured microphone levels and elapsed
time. The destination appears briefly at the start. Warnings and recovery
states expand in place. The pill is click-through, never takes keyboard focus,
and never shows the transcript. Transcription
is staged in the selected Pi prompt for review; recording never
automatically presses Enter. Capture starts only after microphone samples
arrive and stops after three minutes. A pause, or thirty seconds of
uninterrupted speech, flushes a slice into the Pi prompt while recording
continues, so earlier words are kept if the take hits the limit. Silence,
punctuation-only output and non-speech markers are skipped. The recorder already
splits on pauses; a small vocabulary prompt helps with names such as Herdr,
Qwen, NixOS and the configured hosts.

The usual flow uses Super+Space to start, stop, then send. Text may already be
in the prompt before you stop. Wait for the green ready state before sending.
If the selected agent is busy, the take is kept and sent when that agent goes
idle. Super+Space while it is busy queues that send instead of dropping the draft. The hotkey applies across all four launchers and never
broadcasts to other registered sessions, including multiple sessions of the
same harness.

This decision tracks prompts prepared by voice, including subsequent edits.
Use **Super+Shift+V** Record more or `pi-voice record` to add more speech
instead of sending a prepared prompt.

## Keyboard picker

On Andromeda and Foundation, **Super+Shift+V** opens `voice-menu`, a short-lived
Fuzzel popup. Type to fuzzy-filter, use Up/Down, press Enter to choose, or Escape
to close without changing anything. Mouse input is disabled. Choose **session**, **voice** (Qwen characters, Samantha by default) or
**dictation** (Whisper locally, or Deepgram in the cloud). The default
dictation backend is Whisper. Deepgram is recognition only; playback stays on
Qwen. Or select one of the same contextual actions: record more, retry, discard, replay, or rebind.
The existing recording and send hotkeys are unchanged; the picker does not
introduce another Send action.

```bash
voice-menu            # controls and submenu choices
voice-menu voices     # character list; * marks the selected voice
voice-menu dictation  # Whisper or Deepgram; * marks the selected backend
voice-menu sessions   # other currently selectable registered sessions
```

Sessions are selected by their registration token, not their displayed label.
The current session is normally omitted, except when it needs explicit
confirmation for retained-text recovery. Session and dictation switching are
unavailable while capture/transcription is busy. Status is refreshed
before dispatch; stale actions or changed target bindings require reopening the
menu. It never displays dictated drafts or assistant replies.

The dedicated config at `~/.config/voice-menu/fuzzel.ini` is generated by
`home/user/voice.nix`. It includes `~/.local/state/theme-menu/active/fuzzel.ini`,
the same active style included by the default Fuzzel config. Width and lines
come from `menus.voice` in `home/config/hyprland/settings.nix`. It does not
replace the application launcher or run another daemon.

After activation, try the shortcut, filter a voice name and press Escape first.
Then choose a voice and check its `*` marker on reopening. For sessions, launch
two voice-enabled agents, select the other session and confirm the pill names that target.

## Experimental PersonaPlex conversation (Andromeda)

Use **Super+Shift+V → Try PersonaPlex conversation (experimental)** to start
local full-duplex conversation in a browser. The first startup loads a separate
7B model and opens `http://127.0.0.1:8998` once ready. Choose a voice and connect
in that page; browser microphone permission is required. Start with headphones.
This is a standalone conversation, not a Pi coding session: it cannot use Pi's
tools, receive its results, or submit tasks. The existing character voice picker
still controls Qwen TTS, not PersonaPlex.

Reopen **Super+Shift+V** and choose **Switch back to Pi dictation and playback**
to stop PersonaPlex and release its GPU memory and browser microphone streams.
Escape changes nothing. Pi's
record/read hotkeys also return to Pi mode because the services are mutually
exclusive. Close the conversation tab when finished. Conversation mode does not
persist across login and does not launch automatically. Switching is unavailable
while dictation is recording, pending, retained or awaiting retry; finish or
discard it first. Pi coding work itself is not stopped by switching voice modes.

PersonaPlex stops the Pi voice controller and speech engines while active. The
status pill stays up so a startup failure is shown there instead of a desktop
notification. Returning to Pi restores its configured automatic-playback default;
in-memory replay state is not carried across modes. If startup fails, reopen the
voice menu to recover Pi controls. Inspect `journalctl --user -u personaplex`
for the model failure. At least 24 GiB free GPU memory is required before loading;
the service refuses to evict a running coding model automatically.

Initial setup requires accepting the NVIDIA model terms at
<https://huggingface.co/nvidia/personaplex-7b-v1> and configuring Hugging Face
access through `HF_TOKEN` or the standard cached token. Run `personaplex-models`
to download about 17.1 GB of pinned, SHA-256-verified assets into the private voice
data directory. Credentials and gated weights never enter the Nix store or Git.
`personaplex-models --check-only` verifies installation without network access.
Normal service startup is offline. The server binds only to loopback and accepts
conversation WebSockets only from its local browser UI origins. PersonaPlex
requires NVIDIA CUDA and is not offered on Foundation or other hosts.

## Status pill and recovery

**Super+Shift+V** is the only voice menu. It contains session, voice, dictation,
show team members and read replies aloud. Team panes are not registered, so that
toggle does not reveal them. Dictation is a radio choice between local Whisper
and cloud Deepgram. It is locked while recording or transcribing so the current
take keeps one backend. **Read replies aloud** toggles automatic playback of
completed replies. Other actions appear only when useful: Replay last reply,
Record more, Retry transcription, Discard retained dictation/recording, and Bind
to current conversation. Cancel recording, Cancel transcription or Stop speaking
appears while that voice work is active. These controls do not cancel a running
coding-agent turn. Super+Space handles the normal record/transcribe/send flow.
There is no StatusNotifier icon.

The top pill is the only voice message. It does not raise a separate desktop
notification, and it never shows the transcript or the reply. Its charcoal
surface stays consistent across themes; the status indicator uses the active
theme's colour roles. Silence produces a flat meter, not a microphone error.
Clipping must persist beyond the capture peak latch before it raises a warning;
the first two seconds of capture have a warning grace period. Muting is immediate.

Editing the staged text in Pi hides **Ready to send** and cancels automatic
queued submission within the next background status check (about one second).
Explicit Send still accepts a corrected draft. Clearing or manually submitting
the prompt disarms that voice draft; later unrelated typing cannot be submitted
as if it were still the cleared dictation. Only editor-state flags leave Pi,
never the editor text.

| Pill | Colour role | State |
| --- | --- | --- |
| Capture light, meter and timer | Red | Recording |
| Microphone muted / Input too loud | Orange | Capture needs attention |
| Starting, finishing, transcribing | Yellow | Microphone or recognition in progress |
| Ready to send / Will send when idle | Green | Dictation is prepared |
| About to speak | Accent | Reply is queued and speech has not started |
| Speaking | Teal | Reply audio is playing |
| Needs attention, retained, model failure | Orange | Something needs a menu action |
| Reconnecting / conversation changed | Yellow | Destination is not ready |

The capsule uses GTK4/Cairo on the existing Wayland layer-shell surface, with
no browser runtime. Socket and monitor queries run off the GTK event thread.
GTK's reduced-motion setting disables geometry easing and spinner rotation.
Hidden and settled non-working states stop requesting animation frames. If the
controller connection is lost during recording, the pill reports that microphone
state is unknown instead of showing stale audio activity.

Dictation outranks speech. A working agent does not raise the pill; it appears
when speech is actually queued. Pi lifecycle events update activity promptly;
bounded background reads confirm completion about once a second. Slow or
unavailable harnesses cannot block the menu or voice cancellation. Controller
reconnection retries quietly. Warm-up is backend work, not a model prompt or
spoken reply, so model loading alone does not raise the pill.

Andromeda prefers the Razer Kiyo Pro Ultra's stable device
name, with a visible fallback to PipeWire's default if absent. Speakers follow
the PipeWire default.

Recording works while the selected agent is busy. Valid text waits in memory
until it can be safely delivered. A new recording appends to retained text by
default. `pi-voice replace` replaces retained text only after valid
new speech, so silence or a failed start preserves previous words. Discard
removes retained text and retry audio. Cancel does not remove text already
pasted into a terminal editor; that prompt remains available for review.

If a Pi destination is lost or replaced while dictation is pending, the pill
names its source. **Super+Shift+V** offers **Copy retained dictation**, **Stage in selected Pi
session**, or **Discard retained dictation**. Staging requires a ready, idle Pi
destination and preserves its editor text. If the current destination was
selected automatically, first choose **Confirm <label> for retained dictation**
in the session selector, even when it is the only session. Confirmation only
selects the destination; then choose **Stage in selected Pi session**.
Recovery never submits automatically:
review the staged text and send explicitly. Retained text is memory-only and
is not silently redirected to another session.

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

Standard Pi discovers `pi-voice.ts` from its managed extensions directory;
`qwen-pi` explicitly loads the same extension because automatic discovery is
disabled. Auto-attachment is limited to supported hosts and interactive Herdr
sessions that are not team panes. It uses native editor APIs for staging and
single-use submission, preserving existing typed text. Super+Space explicitly
submits the current edited voice draft. The separate Send action keeps its
unchanged-editor guard; after that guard rejects edits, submit in Pi or record
again. Spoken replies use `agent_settled`, excluding
reasoning, aborted output and intermediate tool turns. Adapter sockets are
private and check process and conversation identity. Team children never open
a bridge.

Registration metadata survives a controller restart while the processes live.
Retained dictation, prior replies, WAV retries and playback preferences do not.
The registry never persists prompt or reply contents.

## Speech and services

**Super+Shift+V** selects Samantha or another character.
Samantha automatically uses the current reference for up to 50 words and the
newer reference for longer replies. This is intrinsic to Samantha; other
characters use their own reference at every reply length. The complete spoken
reply is counted after Markdown cleanup and keeps one voice across all chunks.
Selections apply on the next playback and persist across service restarts.
Use `pi-voice voice samantha`, `pi-voice voice data`, or another character
ID for the same control from a terminal. `pi-voice status` lists the IDs.
The choice is shared by Pi and Qwen Pi. Two prompt cache slots retain
recent references without loading additional models. Previous saved Samantha
modes migrate to the single Samantha selection.

The catalogue includes JARVIS, HAL 9000, Joi, Data, KITT, Galadriel, Avasarala,
Spock, Picard, Snape, Seven of Nine, Kryten, Holly, Loki and Vesper. These are
experimental audition references, with source and transcript limitations
recorded in [the catalogue](../home/config/voice/voices/catalogue.json).

Spoken summaries play automatically. Use `pi-voice auto off` for manual playback,
`auto on` to restore it, `stop` to cancel and `status` to inspect state. All four
launchers accept the same controls: `interact`, `record`, `send`, `read`, `stop`,
`status`, `retry`, `rebind`, `discard`, `append`, `replace` and `voice`.

### Summary-only playback

The complete written answer stays in the coding TUI. Automatic speech and manual
replay consume only its final `## Summary` section, using the same shared
controller for Pi and Qwen Pi. `Spoken summary`, `TL;DR` and `TLDR`
are accepted aliases; plain colon labels, bold labels and Markdown headings work
too. For example:

```markdown
The full response can contain detailed explanations, lists, paths and code.

## Summary
The fix is in place and the tests passed. Live microphone testing is still
needed. Next, try a voice session to check how the summary sounds.
```

Shared agent instructions request conversational prose, usually two to four
short sentences: outcome, important caveat, then next action. Short answers can
use one sentence. They target 30 to 80 words without lists, paths, commands or
URLs. Exact-output requests such as JSON-only responses take precedence and do
not need a summary.

The controller accepts at most 120 words and 1500 characters after cleanup.
Missing, empty, oversized or non-terminal summaries stay silent; there is no
full-answer fallback or extra summarization model call. A completion without a
summary also clears the previous summary so Replay cannot read stale results.
Manual Read reports that the latest reply has no summary. Fenced code is
ignored during extraction, and common Markdown/list markers are stripped as a
defensive cleanup, not a substitute for writing prose.

After switching Home Manager, restart the controller if it has not restarted
with the new generation. Existing voice launchers use the shared controller, but
agent instruction changes may require a fresh session or the agent's instruction
reload mechanism before summaries appear. Old unlabelled replies are not read.

Speech prefers sentence and clause boundaries with a shorter first phrase and
a 260-character maximum.
Andromeda synthesizes one chunk ahead during playback through one continuous
PipeWire stream. These are complete-reply playback modes, not live reading of
unfinished model text.
Recording interrupts speech; a reply completing during recording waits for
manual playback.

The existing service names remain for compatibility:

- `pi-voice.service`: shared controller and hotkeys.
- `pi-voice-stt.service`: Whisper at `127.0.0.1:8178`.
- `pi-voice-tts.service`: Qwen3 TTS at `127.0.0.1:8179`.

The controller starts at login; implicit Pi use starts only the required
backend. Explicit compatibility launchers still warm both engines. Models stop
after 15 idle minutes measured from completed use, never during active work.
Warm-up runs in the background. Ports must be available. The default models
occupy about 2.93 GB in `~/.local/share/pi-voice/models`. Run
`pi-voice-models` on a fresh host, and `pi-voice-models --check-only` to
verify sizes and hashes. Silero VAD is also fetched with a fixed hash by Nix,
so enabling it requires no extra setup. Inference uses local
files without cloud speech APIs or runtime Python package downloads. Prompt
text still goes to the provider selected by the coding harness.

The voice reference, transcript and provenance are in
[`home/config/voice/voices`](../home/config/voice/voices/README.md). Runtime
state, private adapter sockets and temporary recordings use
`$XDG_RUNTIME_DIR/pi-voice`. Whisper's diagnostic journal can contain
recognized text; the TTS server disables request-body logging.

Stop the services explicitly with:

```sh
systemctl --user stop pi-voice.service pi-voice-stt.service pi-voice-tts.service
```

Inspect failures with:

```sh
journalctl --user -u pi-voice -u pi-voice-stt -u pi-voice-tts -n 100
```

On Andromeda, pull the configuration, run `hmswitch`, then
`pi-voice-models` and verify the microphone, NVIDIA Vulkan and playback.
On Foundation, `hmswitch` then `pi-voice-models` installs Whisper small.en only
(about 500 MB). Terminus and Relay are excluded. The macOS/Relay browser setup remains text-only.

## Voice choice and measured performance

The model and `default_voice_preset` are set in `home/user/voice.nix`.
The default preset references the committed short-reply WAV and its transcript.
The controller supplies the newer pinned WAV and transcript for long Samantha
replies, or the selected character reference. Model weights
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

These measure local inference, excluding model response time, hotkey dispatch
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
`home/user/voice.nix` selects `streaming` for Andromeda. Neither mode loads
another model onto the GPU.

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
Matching decoder graphs remain cached. See [local Qwen](local-llm.md) for the shared memory
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
`pi-voice-models --experimental --check-only` verifies them. Audition files
are in `~/.local/share/pi-voice/voices`, including
`qwen3-0.6b-samantha-preview.wav`, `qwen3-1.7b-samantha-preview.wav` and
`qwen3-0.6b-samantha-prose-preview.wav`.

## Development checks

```sh
direnv exec . node --test tests/*.test.ts
direnv exec . nix build \
  '.#homeConfigurations."william@andromeda".activationPackage' --no-link
direnv exec . nix build \
  '.#homeConfigurations."william@foundation".activationPackage' --no-link
direnv exec . nix flake check
```

Run the voice regressions with:

```sh
direnv exec . python3 -m unittest discover -s tests -p 'test_voice*.py' -v
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
and socket framing. Isolated installed-harness checks verify Pi extension loading.
Physical microphone and listening trials
are still needed to assess recognition and speech quality.
Capture tests use real subprocess fixtures for startup timeout, cancellation,
stalls, disconnects, partial PCM reads and file-write failures. Tray integration
uses a private D-Bus session to check registration, icon changes, menu actions,
watcher replacement and responsiveness during blocked controller status reads.
