# YouTube extract details

Before drafting or updating prose, read
`~/.agents/guides/documentation-relevance.md`.

## Command

From any checkout of this skill:

```bash
python3 scripts/youtube_extract.py \
  'https://www.youtube.com/watch?v=VIDEO_ID&t=1051s'
python3 scripts/youtube_extract.py \
  --url URL --around 17m31s --window 90 --query qwen
```

The script calls `yt-dlp --skip-download --no-playlist`, writes info JSON and
English VTT into a temp dir, then prints metadata plus a caption slice. It never
passes `-f`, `-x`, or an audio/video output.

`--subs-file` and `--info-file` are offline fixtures for tests. Do not use them
instead of a live fetch for a real URL.

## What to read

| User intent | Read |
|---|---|
| Named timestamp or `t=` | Description, then captions in the window |
| "What model / tool / paper" | Description, chapters, then `--query` |
| Quote or paraphrase | `--query` with distinctive words |
| Whole video summary | Description and chapters; sample captions if needed |

Auto-captions overlap. The script drops rolling duplicates. Proper nouns in
auto-captions are unreliable; prefer the description when they disagree.

## Failures

- No `yt-dlp` and no Nix: stop and say the extractor cannot run.
- Empty captions: use description and chapters; do not invent speech.
- Sign-in, age-gate, or bot check: report the `yt-dlp` error. Optional last
  resort is cookies from the visible Brave profile, only if the user asks.
- Non-English captions: pass a different `--sub-langs` only when the user
  needs that language; default remains English.

Do not add `yt-dlp` to Home Manager or flake inputs for a one-off extract.
