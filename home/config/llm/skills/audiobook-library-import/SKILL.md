---
name: audiobook-library-import
description: Audiobook acquisition review and safe library import workflow for William's Andromeda and Terminus setup. Use for qBittorrent audiobook transfers, NAS audiobook staging, Audiobookshelf metadata or duplicate cleanup, William/Children library classification, per-book acceptance checks, or when the user runs /audiobook-library-import.
---

# Audiobook Library Import

Use this skill for moving and reviewing audiobook payloads from Andromeda into
the Audiobookshelf libraries on Terminus.

Read `references/audiobook-library-import.md` completely before inventorying,
copying, deleting, importing, or changing Audiobookshelf metadata.

## Non-negotiable workflow

1. Inventory qBittorrent payloads from its fast-resume records. Do not assume
   every directory in `~/Downloads` is in scope.
2. Copy selected payloads into a dated directory below
   `/srv/media/imports/`, never directly into an Audiobookshelf-watched root.
3. Re-run the transfer incrementally, then require a full checksum dry-run
   before treating the source machine as safe to suspend.
4. Account for every payload as `accept`, `duplicate`, or `reject`, with
   evidence and a destination library for accepted books.
5. Apply the per-book acceptance criteria from the guide. Do not equate bitrate
   alone with quality.
6. Prepare the final one-book-per-folder layout outside watched roots. Compare
   staged and prepared checksums before an atomic move into the library.
7. Let exactly one ingestion mechanism run: the watcher or an explicit scan.
   Never deliberately race both.
8. Resolve Audiobookshelf item IDs from the exact path immediately before each
   API mutation, then verify the returned path and title. Do not write directly
   to the SQLite database.
9. Finish with checksum, database, metadata, duplicate-path, and payload
   accounting checks before deleting any source or staging data.

## Safety boundaries

- Preserve unrelated downloads, existing library files, listening progress,
  sessions, and alternate editions unless the user explicitly puts them in
  scope.
- Skip exact duplicates. Treat work-level duplicates as a judgement call based
  on narrator, edition, duration, and the user's stated preference.
- Reject incomplete or corrupt payloads instead of repairing or silently
  importing them unless the user asks for recovery work.
- Never print Audiobookshelf tokens. Read the local database only to discover
  state or obtain a token for an API call.
- Use the documented Audiobookshelf API for metadata and item deletion.
  Deleting a database item is not permission to delete its media files.
- Report uncertainties explicitly; default ambiguous adult/child classification
  to William's library pending evidence.

## Required handoff

Report:

- source payloads, accepted payloads, duplicates, and rejects;
- imported books, files, and bytes per library;
- duplicate/reject reasons;
- validation failures or metadata uncertainties;
- whether the source-to-staging and staging-to-library checksums matched;
- whether Audiobookshelf contains exactly one valid item per imported path.
