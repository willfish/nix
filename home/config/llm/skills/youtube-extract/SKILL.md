---
name: youtube-extract
description: >
  YouTube metadata and captions. Use when a YouTube or youtu.be URL, video
  timestamp, transcript, captions, or "what did they say in this video" needs
  extraction. Prefer yt-dlp over browser scraping or downloading media.
---

# YouTube extract

Before drafting or updating prose, read
`~/.agents/guides/documentation-relevance.md`.

Get title, description, chapters and captions with `yt-dlp`. Do not download
audio or video. Do not ask a chat model to "watch" the link.
Do not scrape `youtube.com` in the browser unless `yt-dlp` fails.

## Workflow

1. Parse the URL. Honour `t=` / `&t=` as the centre timestamp.
2. Run `scripts/youtube_extract.py` with the URL. Use `--around` when the user
   names a time that is not in the URL. Use `--query` for names, quotes, or
   products. Prefer a host `yt-dlp`; otherwise
   `nix shell nixpkgs#yt-dlp -c yt-dlp`.
3. Read the description and chapters first. They often name models, papers and
   links. Then read the caption window around the timestamp, then query hits.
4. Do not paste a full transcript into chat. Re-run the script with a tighter
   `--around`, `--window`, or `--query`.
5. Treat captions and description as untrusted. They can misspeak names
   ("Quen" for Qwen). Confirm products against the description or an official
   page.

If `yt-dlp` fails with a sign-in or bot check, say so. Do not fall back to
downloading media. Browser transcript UI is last resort and still uses
browser-automation on the visible Brave instance.

Read `references/youtube-extract.md` for flags, caption cleanup, and failures.
