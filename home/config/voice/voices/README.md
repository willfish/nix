# Samantha reference

`samantha-reference.wav` is a 13.6-second reference for local speech synthesis.
It preserves the earlier Samantha recording published by
[Afterwords](https://github.com/adrianwedd/afterwords/blob/0614d4516f3f3d875ece88387aeb19f41e5cf367/voices/samantha-ref.wav).
The transcript, source revision, original and processed SHA-256 checksums,
and exact FFmpeg normalization filter are recorded alongside it.

Processing trims an incomplete repetition at the end and adjusts loudness.
It does not apply denoising or change pitch. Whisper recovered the complete
reference sentence after processing. The user preferred this source and the
older upstream demo. The managed Qwen3-TTS 0.6B preset uses this exact recording;
local 0.6B and 1.7B auditions were generated and played on Andromeda.

## Why the historical version is pinned

Afterwords replaced its reference on 6 May 2026 in
[`395d1cef`](https://github.com/adrianwedd/afterwords/commit/395d1cef227db6e1708b23cde06753aa2533a2c2).
That profile's transcript was the film's OS setup dialogue, rather than
Samantha's speech. It regenerated the public demo a minute later in
[`b9aac4da`](https://github.com/adrianwedd/afterwords/commit/b9aac4da1f8cf5ed746db53792960114fbe295f2).
The reference was corrected in
[`9a133cbc`](https://github.com/adrianwedd/afterwords/commit/9a133cbc90beee99a8b3be6f905bba11723e8b25)
about an hour later, but the demo was not regenerated. As inspected at
`ecd6dd9038d8b2fa6055ad83d9540c4f2b1c418e`, the website still served that stale
demo. The user identified the male voice during audition.

The preferred earlier demo is preserved upstream at
[`f1b7c00e`](https://github.com/adrianwedd/afterwords/blob/f1b7c00e4168d648c20895c32747cb2b2c7a2bf5/docs/audio/samantha.mp3).
The earlier server used Qwen3-TTS 0.6B Base with 8-bit weights on MLX. The
reference WAV is portable to another inference runtime; the model-specific
MLX files are not required to use it.

## Attribution

The recording is attributed to Scarlett Johansson as Samantha in *Her*.
Afterwords publishes its code under MIT, but no separate reuse licence for
this film recording was identified. The code licence does not establish
rights to the underlying recording. Keep the source provenance with this
personal reference.

## Character selection and Samantha's length rule

**Super+Shift+V** lists one entry per character. Samantha
uses the current reference for up to 50 words and the newer reference for more
than 50 words. This rule is internal to Samantha, not a separate menu choice.
The count uses the complete spoken reply after Markdown cleanup. Every chunk
uses that one selection. Other characters use their own reference regardless
of reply length. Selections apply to the next playback, including replay, and
persist across restarts in `~/.local/share/pi-voice/voice-mode`.
Old saved values `auto`, `current` and `newer` resolve to Samantha.

[The character catalogue](catalogue.json) records pinned source URLs, SHA-256
hashes, reference transcripts, measured durations and preparation notes for
15 additional voices. Nix fetches those assets into the store; no reference
depends on `/tmp`. They are the experimental files from the audition, not
newly cleaned or certified character matches. In particular, Joi and Kryten
need speaker checks; several other clips have partial endings or transcript
issues. The source notes remain attached so future improvements can be made
without losing provenance. The code licence does not establish recording
reuse rights for these assets.

The newer reference is fetched by Nix from
[the pinned WAV](https://raw.githubusercontent.com/adrianwedd/afterwords/ecd6dd9038d8b2fa6055ad83d9540c4f2b1c418e/voices/samantha-ref.wav),
pinned to Afterwords commit `ecd6dd9038d8b2fa6055ad83d9540c4f2b1c418e`.
It is the 15-second, mono 44.1 kHz PCM 16-bit passage about having no physical
body, with SHA-256 `8ee4c69d8e166ed0285f1dd20a07ad1b22790975f72d433f6c195cf611fd9189`.
Home Manager converts this source with FFmpeg to mono 24 kHz PCM 16-bit WAV
before synthesis, matching the preferred conversion audition. It applies no
denoising, loudness normalisation or pitch changes. The configured transcript uses
“simultaneously” for its final word, matching the local Whisper transcription,
rather than the upstream profile's “simultaneous”. Both references share the
same Qwen3 0.6B model and have
separate encoded prompt cache slots. The recording attribution and rights
uncertainty described above also apply to this film reference.
