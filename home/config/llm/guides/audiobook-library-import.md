# Audiobook Library Import: Andromeda to Terminus

This is the concrete runbook for reviewing audiobook downloads on Andromeda,
transferring them to Terminus, and importing them safely into Audiobookshelf.

## Contents

- [Setup](#setup)
- [Definition of done](#definition-of-done)
- [Phase 1: inventory the source](#phase-1-inventory-the-source)
- [Phase 2: copy to staging](#phase-2-copy-to-staging)
- [Phase 3: review every payload](#phase-3-review-every-payload)
- [Per-book acceptance criteria](#per-book-acceptance-criteria)
- [Collections and multi-story payloads](#collections-and-multi-story-payloads)
- [Phase 4: prepare and import](#phase-4-prepare-and-import)
- [Phase 5: Audiobookshelf verification and metadata](#phase-5-audiobookshelf-verification-and-metadata)
- [Phase 6: final accounting](#phase-6-final-accounting)
- [Worked examples from the July 2026 import](#worked-examples-from-the-july-2026-import)
- [Operational pitfalls](#operational-pitfalls)

## Setup

### Hosts and paths

| Purpose | Location |
|---|---|
| qBittorrent state on Andromeda | `/home/william/.local/share/qBittorrent/BT_backup/*.fastresume` |
| qBittorrent download root | `/home/william/Downloads` |
| Terminus import staging | `/srv/media/imports/<descriptive-date>` |
| William library | `/srv/media/audiobooks` |
| Children library | `/srv/media/audiobooks-children` |
| Celine library | `/srv/media/audiobooks-celine` |
| Phone library | `/srv/media/phone-audiobooks` |
| Audiobookshelf | `http://127.0.0.1:13378` |
| Audiobookshelf database | `/var/lib/audiobookshelf/config/absdatabase.sqlite` |

Audiobookshelf library IDs observed in July 2026 were:

| Library | ID |
|---|---|
| William | `2e14d8b6-d18e-447d-8dcd-1a6c9ab48db0` |
| Children | `efb788f1-4f4d-4bf5-a9e4-73ae56d752c3` |

Treat IDs as discoverable state, not constants for mutation. Resolve the
current library and item IDs by name and exact path before making API calls.

### Tooling

Use existing tools first. Run missing one-off tools ephemerally through Nix:

```bash
nix shell nixpkgs#sqlite -c sqlite3 --version
nix-shell -p 'python3.withPackages (ps: [ ps.bencoder ])' \
  --run "python -c 'import bencoder'"
```

Do not persist packages in the dotfiles merely to perform an import.

## Definition of done

An import is complete only when:

- every selected source payload is classified as accepted, duplicate, or
  rejected;
- every accepted book passes all applicable per-book criteria;
- source-to-staging and prepared-to-live checksums agree;
- duplicates and rejects are absent from the live library;
- Audiobookshelf has exactly one valid item for every imported path;
- titles, authors, narrators, covers, durations, and audience placement have
  been checked;
- no pre-existing listening history or unrelated files were removed;
- the final report reconciles payload, book, file, and byte totals.

Do not delete source or staging data until all these conditions pass.

## Phase 1: inventory the source

The source of truth for download scope is qBittorrent's fast-resume data, not a
recursive copy of `~/Downloads`. The latter can contain unrelated or private
files.

Decode each `.fastresume` record, extract its `name`, join it to the configured
download root, and retain only paths that currently exist. `bencoder` may be
run ephemerally with Nix. Produce a NUL-delimited file list so spaces,
apostrophes, and newlines cannot corrupt the transfer.

Example shape:

```bash
ssh andromeda \
  "nix-shell -p 'python3.withPackages (ps: [ ps.bencoder ])' \
    --run 'python /path/to/inventory.py'" \
  > /tmp/andromeda-audiobooks.list0
```

The inventory helper should:

1. read `/home/william/.local/share/qBittorrent/BT_backup/*.fastresume`;
2. decode `b'name'`;
3. reject absolute paths and `..` traversal;
4. test the joined path beneath `/home/william/Downloads`;
5. emit existing relative paths separated by NUL;
6. report absent payloads separately rather than fabricating source paths.

Record an initial manifest with relative path, type, size, and modification
time. Add checksums either before the copy or during the checksum verification
pass.

## Phase 2: copy to staging

Create a dated staging directory outside every Audiobookshelf-watched root:

```bash
mkdir -p /srv/media/imports/andromeda-audiobooks-YYYY-MM-DD
```

Use the NUL-delimited inventory with `rsync`:

```bash
ssh andromeda 'cd /home/william/Downloads && <emit-list0>' |
  rsync -a --info=progress2 --from0 --files-from=- --protect-args \
    andromeda:/home/william/Downloads/ \
    /srv/media/imports/andromeda-audiobooks-YYYY-MM-DD/
```

Because an active torrent or post-processing job can change files during the
copy:

1. run the initial copy;
2. run the same `rsync` again to catch changes;
3. run a full checksum dry-run;
4. repeat until the checksum dry-run reports no transfer.

Example verification:

```bash
ssh andromeda 'cd /home/william/Downloads && <emit-list0>' |
  rsync -a --checksum --dry-run --itemize-changes \
    --from0 --files-from=- --protect-args \
    andromeda:/home/william/Downloads/ \
    /srv/media/imports/andromeda-audiobooks-YYYY-MM-DD/
```

Only the final no-change checksum result makes it safe to suspend Andromeda.

## Phase 3: review every payload

Create an accounting table before importing anything:

| Source payload | Decision | Destination | Evidence |
|---|---|---|---|
| Book A | accept | William | Complete, decodes, metadata confirmed |
| Book B | duplicate | none | SHA-256 equals existing library file |
| Book C | reject | none | Missing tracks and decode failures |

The number of rows must equal the number of selected source payloads.

Review payload structure and supporting material:

- enumerate audio, cover, cue, chapter, metadata, and text files;
- flag temporary, partial, sample, executable, archive, and unrelated files;
- check sequence numbers for gaps;
- distinguish one book split into tracks from a collection containing many
  individually discoverable books;
- retain useful cover and metadata files with accepted books.

## Per-book acceptance criteria

Apply every applicable criterion to each book. A book passes only when all
required rows pass or a deviation is documented and approved.

### AC1: identity and edition

- The spoken title and author agree with the proposed metadata.
- The narrator or cast is identified when available.
- The edition is distinguishable from existing alternatives: unabridged,
  abridged, dramatisation, radio adaptation, language, or narrator.
- An ASIN/ISBN/product identifier is recorded when reliable.
- Metadata does not claim a different narrator or production.

For Harry Potter specifically, a full-cast dramatisation must never carry
Stephen Fry edition metadata. Include the production type or cast in the title
or subtitle when that is needed to make the children's-library picker clear.

### AC2: completeness

- Every expected audio file is present.
- Numbered tracks or chapters form a contiguous sequence unless the source
  edition proves otherwise.
- Each file has at least one readable audio stream and a positive duration.
- The beginning contains the expected opening, not a mid-sentence start.
- The end reaches a natural conclusion or closing credit and is not truncated.

Missing numbered lectures, chapters, or stories fail this criterion even when
the remaining files play.

### AC3: technical integrity

Probe every audio file:

```bash
ffprobe -v error -show_entries \
  stream=codec_name,channels,sample_rate:format=duration,bit_rate \
  -of json "/path/to/file"
```

Then decode the entire audio stream and treat warnings promoted by `-xerror` as
failures:

```bash
ffmpeg -v error -xerror -i "/path/to/file" -map 0:a:0 -f null -
```

All files must decode successfully. A tag parser succeeding is not evidence
that the complete audio stream is healthy.

### AC4: listening quality

- Sample the beginning, middle, and end of the book.
- For multi-file books, sample multiple files including the first and last.
- Speech is intelligible at normal playback speed.
- There is no severe clipping, persistent corruption, excessive noise, long
  unintended silence, inserted advertising, piracy-channel branding, or
  unrelated content.
- Loudness is usable without extreme gain changes between files.
- The narrator and content heard match AC1.

Bitrate is evidence, not a pass/fail threshold. A low-bitrate Opus speech
encoding can be better than a higher-bitrate damaged MP3. Consider codec,
sample rate, source age, speech clarity, and the availability of alternatives.

Speech recognition can help identify lightly tagged recordings, but it does
not replace listening. Transcribe short samples locally and verify uncertain
names against reliable catalogue or publisher evidence.

### AC5: duplicate status

Search all relevant live library roots and Audiobookshelf before importing:

- normalized title and author;
- ASIN/ISBN where present;
- narrator, cast, production type, duration, and chapter count;
- same-size candidates followed by a cryptographic checksum;
- short listening comparisons for re-encoded copies.

Classify duplicates:

| Type | Default action |
|---|---|
| Exact file checksum | Skip the incoming copy |
| Same recording, re-encoded | Keep the better copy; do not import both |
| Same work and same narrator/edition | Skip unless one has a clear quality advantage |
| Different narrator or production | Keep only when the distinction is intentional and clearly labelled |
| Existing item has listening history | Preserve it; do not replace or delete casually |

The user's explicit instruction to drop duplicates permits skipping incoming
copies. It does not permit deleting an existing library copy.

### AC6: audience and destination

- Put material intended for children or young adults in Children.
- Put adult fiction, non-fiction, satire, lectures, self-help, and general
  science in William unless the user says otherwise.
- Do not infer audience solely from cover style or a familiar franchise.
- When evidence is genuinely ambiguous, default to William and report the
  uncertainty.
- Preserve Celine and Phone as separate scopes; do not import into them without
  an explicit reason.

### AC7: filesystem layout

- Store one Audiobookshelf book per leaf folder.
- Prefer `{Author}/{Series}/{Book}` when series information is useful.
- Keep all tracks, cover art, chapters, and book-level metadata inside that
  book's folder.
- Do not leave nested unrelated books beneath one parent item.
- Prepare the full structure outside the watched roots.
- Compare prepared files to staging, then atomically rename or move the
  completed leaf/directories into the live root.

### AC8: Audiobookshelf record

After ingestion:

- exactly one library item resolves to the exact live path;
- the item is not missing or invalid;
- duration is positive and consistent with the files;
- title and author are correct;
- narrator/cast and edition type are present when needed to distinguish copies;
- a suitable cover is displayed;
- series and sequence are correct when applicable;
- no new duplicate-path record exists;
- metadata edits are reflected both in the API response and a fresh read.

Resolve an item ID by exact path immediately before each mutation. Never copy a
batch of IDs from a report and assume row order remains stable.

## Collections and multi-story payloads

A collection needs both collection-level and child-level checks:

- decide whether Audiobookshelf should expose it as one compilation or many
  books;
- ensure every child has its own leaf folder when separate discovery is
  desired;
- verify the count of unique child paths and database items;
- check consistent author, series, title, cover, and numbering metadata;
- sample several children across the set, not only the first;
- look for source-tag leakage into author fields and systematic spelling
  errors.

Roger Hargreaves is the concrete precedent: the source hierarchy became 96
individual Children's books. Verification found an author tag of `Plan B` on
`Little Miss Tiny` and title typos such as `My Nosey`, `Mr Skiny`, and
`Mr Upity`; collection-level counts passed only after those per-book defects
were corrected.

## Phase 4: prepare and import

Build accepted destinations beneath a temporary preparation root on the same
filesystem as the live library when possible. Same-filesystem renames are
atomic, so Audiobookshelf cannot observe a half-built book.

Before moving:

1. compare each prepared file against its staged source by checksum;
2. confirm no destination path already exists;
3. record book, file, and byte counts;
4. move accepted directories into the selected live root;
5. leave duplicates and rejects in staging.

Do not copy directly into `/srv/media/audiobooks*` while validation or metadata
work is still in progress.

## Phase 5: Audiobookshelf verification and metadata

Audiobookshelf's watcher is enabled on this setup. Choose one ingestion path:

- move the books and wait for the watcher to settle; or
- perform a controlled explicit library scan when no watcher scan is active.

Do not trigger an explicit scan while the watcher is ingesting the same files.

### API authentication

Obtain the root token without printing it:

```bash
db=/var/lib/audiobookshelf/config/absdatabase.sqlite
token="$(
  nix shell nixpkgs#sqlite -c sqlite3 -readonly "$db" \
    "SELECT token FROM users WHERE username='root';"
)"
```

Use it only in an authorization header:

```bash
curl -fsS \
  -H "Authorization: Bearer $token" \
  "http://127.0.0.1:13378/api/libraries"
```

Never put the token in logs, reports, committed files, or shell tracing.

### Mutation discipline

For every metadata update or database-item removal:

1. query the API or read-only database for the exact live path;
2. require exactly one matching item;
3. capture its current path, title, and listening-history state;
4. make the documented API request;
5. fetch the item again by ID;
6. assert that the path is unchanged and the title/metadata now match the
   intended book.

Use the API for mutations. The SQLite database is suitable for read-only
verification and token discovery, not direct edits.

If duplicate database records appear, distinguish a scanner race from duplicate
media. Remove only the unwanted database item through
`DELETE /api/items/<ID>` and only after confirming it has no progress or
sessions. Do not delete media files as a side effect unless explicitly asked.

## Phase 6: final accounting

Run fresh checks after Audiobookshelf has settled:

1. source payload rows equal accepted + duplicate + rejected payloads;
2. every accepted source file has an identical live checksum;
3. duplicate and rejected payloads have no live destination;
4. live file and byte totals match the prepared manifest;
5. every imported path maps to exactly one Audiobookshelf item;
6. no imported item is missing, invalid, zero-duration, authorless, or
   coverless;
7. narrator/edition distinctions are clear in library browsing;
8. no new duplicate path exists;
9. pre-existing progress and sessions remain attached to their original item.

The completion report should state all counts and name every exception. Keep
staging until the user accepts the report or explicitly asks for cleanup.

## Worked examples from the July 2026 import

### Source changed during transfer

`Early Modern Philosophy` changed while the first copy was running. The
incremental `rsync` caught the new state, and a second full checksum dry-run was
required before Andromeda was considered suspend-safe. A completed first pass
alone would have produced false confidence.

### Exact duplicates

`The Tree of Life` and `The Twilight Zone Radio (2002-2012) BBC R4` matched
existing files by cryptographic checksum, so their incoming copies were not
placed in the live library.

### Work-level duplicate

`Dirty Beasts` was already represented in the library. It was skipped under the
user's duplicate rule after comparing the work and edition evidence; an
incoming file need not be byte-identical to be an unwanted duplicate.

### Rejected incomplete and corrupt course

`The Great Courses - Great Figures of the New Testament` lacked lectures 10,
12, 18, and 21. Its 20 present MP3s also failed full decode checks, with two
files lacking usable stream metadata. It failed AC2 and AC3 and was not
imported.

### Low bitrate that passed

A low-bitrate Opus recording was retained because complete decoding and
start/middle/end listening samples showed intelligible, clean speech. Rejecting
it on a numeric bitrate threshold would have ignored codec efficiency and the
actual listening result.

### Light metadata recovered from audio

`Restoree` had sparse tags. Its spoken cassette introduction identified the
book and narrator Jill Ferris; that evidence was corroborated before metadata
was applied. Spoken credits are useful evidence, but still require verification.

### Scanner race

Running the watcher and an explicit scan concurrently produced 40 duplicate,
zero-progress database items. They were removed through the documented item
API after confirming there was no listening history. The durable prevention is
to use one ingestion mechanism at a time.

### Wrong item ID caught by immediate verification

A manual metadata update briefly applied `Humans` metadata to `Esoterika`
because an ID copied from a working table was reused. An immediate path/title
read detected and corrected it. Always resolve by exact path at mutation time
and verify after every write.

## Operational pitfalls

- Do not copy all of `~/Downloads`; derive scope from qBittorrent state.
- Do not assume the remote source is static during a long transfer.
- Do not use filename, bitrate, tags, or `ffprobe` alone as acceptance evidence.
- Do not import into a watched root before the book layout is complete.
- Do not run watcher ingestion and explicit scans together.
- Do not mutate Audiobookshelf's SQLite database directly.
- Do not bulk-apply metadata using positional or stale item IDs.
- Do not delete a duplicate database item with progress or sessions.
- Do not clean staging until final checksum and database accounting pass.
