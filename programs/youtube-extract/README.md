# YouTube text extraction

`youtube-extract` extracts metadata and subtitle text, including rolling-caption
deduplication, time windows and casefolded search. It prefers host `yt-dlp`, with
an ephemeral Nix fallback, and requests English subtitles and metadata without
media downloads. Partial caption failures retain metadata and other available
captions. Third-party yt-dlp remains unchanged.

Local fixture inputs never fetch. Temporary downloads are removed; an explicitly
supplied work directory is retained. Build with `nix build .#youtube-extract`.
Manual checks use downloader stubs in the
[native skill command collection](../collections/skill-tools), not live requests.
