---
name: audiobook-library-import
description: >
  Audiobook acquisition, library setup, import, and Audiobookshelf metadata
  workflow for William's Andromeda → Terminus setup. Use for qBittorrent
  download status and Transfers from Andromeda Downloads, Libation Audible
  rips under Music/Libation, NAS staging under /srv/media/imports, creating or
  configuring a new Audiobookshelf library (default Audiobooks William at
  /srv/media/audiobooks), importing into a designated or default library,
  reviewing/explaining/fixing broken metadata via API, duplicate cleanup,
  William/Children classification, per-book acceptance checks, Audiobookshelf
  best practices, or when the user runs /audiobook-library-import.
---

# Audiobook Library Import

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use this skill for the full Andromeda → Terminus audiobook pipeline: source
inventory (qBittorrent and Libation), transfer readiness, staging, review,
import into Audiobookshelf libraries on Terminus, and metadata repair.

## Load references before acting

| When | Read |
|------|------|
| Any inventory, copy, import, or metadata mutation | `references/audiobook-library-import.md` completely |
| Watcher vs scan, library roots, API discipline, layout, safety | `references/audiobookshelf-best-practices.md` completely |
| Deterministic qBittorrent inventory (NUL list / status report) | `scripts/qbittorrent_inventory.py` |
| Source → target duplicate preflight (names/ASIN vs library roots) | `scripts/source_target_duplicate_check.py` |

Do not invent paths or skip safety rules when in a hurry.

## Hosts, roots, defaults

| Role | Host / path |
|------|-------------|
| Source machine (downloads + Libation) | **Andromeda** |
| Library + ABS host | **Terminus** (`services.audiobookshelf`, port **13378**) |
| qBittorrent download root | `/home/william/Downloads` |
| qBittorrent fast-resume | `~/.local/share/qBittorrent/BT_backup/*.fastresume` |
| Libation books (rips) | `/home/william/Music/Libation/Books` |
| Libation state/db | `~/.local/share/Libation/` (`Settings.json`, `FileLocationsV2.json`, `LibationContext.db`) |
| Staging (never a watched root) | `/srv/media/imports/<descriptive-date>` |
| **Default library** | **Audiobooks William** → `/srv/media/audiobooks` |
| Children | `/srv/media/audiobooks-children` |
| Celine | `/srv/media/audiobooks-celine` |
| Phone | `/srv/media/phone-audiobooks` |
| ABS (on Terminus) | `http://127.0.0.1:13378` |
| ABS SQLite (read-only / token only) | `/var/lib/audiobookshelf/config/absdatabase.sqlite` |

Default destination for imports and new-library setup is **William** unless the
user names another library. Do not put new books into Celine or Phone without
an explicit reason.

## Source arms (same later phases)

Both sources land in staging, then share review → prepare → import → metadata:

1. **qBittorrent (Andromeda)** — inventory from fast-resume (not a blind
   `~/Downloads` dump). Check torrent **status** (complete / incomplete /
   seeding / missing path) before rsync. Use
   `scripts/qbittorrent_inventory.py` for status + NUL path lists.
2. **Libation (Andromeda)** — Audible liberations under
   `Music/Libation/Books` (one book folder, typically `.m4b` + sidecar).
   Inventory existing folders and/or `FileLocationsV2.json` paths that still
   exist. Same staging discipline as torrents; never copy secrets or the
   Libation account database to Terminus.

## Non-negotiable workflow

1. Choose source arm(s). For qBittorrent: inventory from fast-resume and
   record download **status**. For Libation: inventory liberated book folders
   that still exist on disk.
2. Do not copy incomplete torrents. Prefer completed + path-present payloads;
   note seeding-but-complete as transfer-ready if files are complete.
3. **Source → target duplicate preflight (required).** Before copying (and
   again before live import), compare each candidate payload against **all**
   relevant Terminus library roots and ABS items:
   - targets: William, Children, Celine, Phone (and any named destination);
   - match on folder/name, ASIN/ISBN, size, then checksum for same-size hits;
   - mark exact/work-level hits as `duplicate` with evidence; do not stage or
     import them unless the user wants a second edition and it is labelled.
   Use `scripts/source_target_duplicate_check.py` plus AC5 in the import guide.
4. Copy only non-duplicate selected payloads into a dated directory under
   `/srv/media/imports/`, never directly into an Audiobookshelf-watched root.
5. Re-run the transfer incrementally, then require a full checksum dry-run
   before treating Andromeda as safe to suspend.
6. Account for every payload as `accept`, `duplicate`, or `reject`, with
   evidence and a destination library (default William). Re-run the
   source→target check on staged paths if anything was copied before the
   first preflight.
7. Apply the per-book acceptance criteria from the import guide. Bitrate alone
   is not quality. Treat criteria as **hard** vs **soft** (see guide): hard
   fails block accept; soft presentation checks are **attempted and reported**,
   never a reason to freeze, invent data, or refuse to hand off.
8. Prepare one-book-per-folder layout outside watched roots. Compare staged and
   prepared checksums before an atomic move into the chosen library root.
9. Let exactly one ingestion mechanism run: watcher **or** explicit scan.
10. Resolve Audiobookshelf item IDs from the **exact path** immediately before
    each API mutation; verify returned path and title. No direct SQLite writes
    for mutations.
11. **Catalogue presentation QA (attempt).** For each accepted/imported item,
    walk quality, naming, language, and cover/image checks (AC9 / Phase 5c in
    the guide). Fix what you can with evidence via the API. For anything you
    cannot resolve (no reliable language signal, ambiguous cover, uncertain
    spelling), record a **residual** and continue — do not invent metadata or
    block the whole batch.
12. Finish with checksum, database, metadata, duplicate-path, presentation
    residuals, and payload accounting before deleting source or staging data.

## New library setup (Terminus / ABS)

When creating or pointing a library:

1. Ensure the filesystem root exists with correct ownership (NixOS tmpfiles
   already create William / Children / Celine / Phone under `/srv/media/`).
2. In Audiobookshelf UI or API: create library type **book**, name it clearly
   (e.g. `Audiobooks William`), folder path = the intended root
   (`/srv/media/audiobooks` for default).
3. Prefer one media folder per library; do not point two libraries at the same
   path.
4. After create: resolve library ID by name via API; store only as discoverable
   state, not a hard-coded constant for later mutations.
5. Confirm watcher settings and do a controlled scan only when the tree is
   stable. See best-practices guide.

## Metadata review → explain → fix

When the user asks about broken or wrong metadata:

1. **Discover** — list library items with missing title/author/narrator/cover,
   zero duration, invalid/missing flags, wrong audience library, bad display
   names, language gaps, or wrong/weak cover art. When the request is about
   “is this already in the library?”, run the same **source → target**
   duplicate check against library roots first.
2. **Explain** — for each item: path, current fields, evidence from tags,
   filenames, spoken credits, or catalogue IDs; state the intended correction.
   For duplicates: name the source path, the target path(s), and match type
   (exact checksum / ASIN / work-level).
3. **Fix** — resolve item ID by exact path; PATCH/update via ABS API only;
   never write SQLite; never bulk-apply IDs from a stale table. Do not delete
   a progressed library item to “make room” for a source duplicate. Prefer
   leaving a field empty over guessing language, narrator, or cover.
4. **Verify** — re-fetch by ID; assert path unchanged and changed fields match
   intent. List residuals you could not close.

Details: import guide Phase 5, 5b, 5c + best-practices guide.

## Hard vs soft criteria (do not corner yourself)

| Tier | Examples | If you cannot fully pass |
|------|----------|---------------------------|
| **Hard** | Incomplete tracks (AC2), decode failures (AC3), exact path collisions, API write without path re-verify | **Reject** or stop that item; do not import corrupt/incomplete media |
| **Soft** | Language tag, perfect cover choice, title capitalisation, narrator spelling without a source, full start/mid/end listen when samples already sound fine | **Attempt**, fix when evidence exists, else **residual in handoff** and continue |

Never invent ASIN/language/narrator/cover to clear a soft check. Never loop
endlessly on presentation polish. Never refuse to report progress because a
soft AC is open.

## Safety boundaries

- Preserve unrelated downloads, existing library files, listening progress,
  sessions, and alternate editions unless the user explicitly puts them in
  scope.
- Skip exact duplicates. Always run source→target preflight before copy and
  before live import. Treat work-level duplicates as a judgement call based
  on narrator, edition, duration, and stated preference.
- Reject incomplete or corrupt payloads instead of silently importing them.
- Never print Audiobookshelf tokens or Libation identity tokens. SQLite is
  read-only for state/token discovery.
- Use the documented Audiobookshelf API for metadata and item deletion.
  Deleting a database item is not permission to delete media files.
- Report uncertainties; default ambiguous adult/child classification to William.
- Soft presentation gaps are residuals, not silent passes and not hard stops.

## Required handoff

Report:

- source arm(s) and payload statuses (complete / incomplete / missing / Libation present);
- source→target duplicate preflight results (matches, match type, target paths);
- accepted, duplicate, and reject counts with reasons;
- destination library (default William if unspecified);
- imported books, files, and bytes per library;
- whether source→staging and staging→library checksums matched;
- metadata fixes applied and re-verified;
- presentation QA notes (naming, language, cover/image, listen quality) per
  book or as a residual list;
- whether ABS has exactly one valid item per imported path;
- residual uncertainties (explicitly mark soft ACs left open).

## Related

- Slash: `/audiobook-library-import`
- Dotfiles / hosts: `local-dev-environment`
- Catalog: `skill-router`
