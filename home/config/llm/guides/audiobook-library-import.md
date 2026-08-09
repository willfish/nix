# Audiobook Library Import: Andromeda to Terminus

This is the concrete runbook for reviewing audiobook sources on **Andromeda**
(qBittorrent downloads and Libation Audible rips), transferring them to
**Terminus**, importing them safely into Audiobookshelf, and repairing metadata
via the API.

Also read `audiobookshelf-best-practices.md` for watcher/scan discipline,
library layout, and mutation safety.

## Contents

- [Setup](#setup)
- [New library setup](#new-library-setup)
- [Definition of done](#definition-of-done)
- [Phase 0: qBittorrent download status](#phase-0-qbittorrent-download-status)
- [Phase 1: inventory the source](#phase-1-inventory-the-source)
- [Phase 1b: Libation source (Andromeda)](#phase-1b-libation-source-andromeda)
- [Phase 1c: source → target duplicate preflight](#phase-1c-source--target-duplicate-preflight)
- [Phase 2: copy to staging](#phase-2-copy-to-staging)
- [Phase 3: review every payload](#phase-3-review-every-payload)
- [Per-book acceptance criteria](#per-book-acceptance-criteria)
- [Collections and multi-story payloads](#collections-and-multi-story-payloads)
- [Phase 4: prepare and import](#phase-4-prepare-and-import)
- [Phase 5: Audiobookshelf verification and metadata](#phase-5-audiobookshelf-verification-and-metadata)
- [Phase 5b: review, explain, and fix broken metadata](#phase-5b-review-explain-and-fix-broken-metadata)
- [Phase 6: final accounting](#phase-6-final-accounting)
- [Worked examples from the July 2026 import](#worked-examples-from-the-july-2026-import)
- [Operational pitfalls](#operational-pitfalls)

## Setup

### Hosts and paths

| Purpose | Location |
|---|---|
| Source machine | **Andromeda** |
| Library / ABS host | **Terminus** |
| qBittorrent state on Andromeda | `/home/william/.local/share/qBittorrent/BT_backup/*.fastresume` |
| qBittorrent download root | `/home/william/Downloads` |
| Libation books (rips) | `/home/william/Music/Libation/Books` |
| Libation app state | `/home/william/.local/share/Libation/` |
| Libation settings / locations | `Settings.json`, `FileLocationsV2.json`, `LibationContext.db` |
| Terminus import staging | `/srv/media/imports/<descriptive-date>` |
| **Default library (William)** | `/srv/media/audiobooks` (**Audiobooks William**) |
| Children library | `/srv/media/audiobooks-children` |
| Celine library | `/srv/media/audiobooks-celine` |
| Phone library | `/srv/media/phone-audiobooks` |
| Audiobookshelf | `http://127.0.0.1:13378` (Terminus; firewall open on LAN/Tailscale) |
| Audiobookshelf database | `/var/lib/audiobookshelf/config/absdatabase.sqlite` |

NixOS on Terminus creates the media roots via `systemd.tmpfiles` in
`system/terminus/configuration.nix` and runs `services.audiobookshelf` on port
13378 with `media` on ZFS `tank/media` mounted at `/srv/media`.

**Default destination** for imports and for “set up a new library” when the user
does not name one is **Audiobooks William** → `/srv/media/audiobooks`.

Audiobookshelf library IDs observed in July 2026 were:

| Library | ID |
|---|---|
| William | `2e14d8b6-d18e-447d-8dcd-1a6c9ab48db0` |
| Children | `efb788f1-4f4d-4bf5-a9e4-73ae56d752c3` |

Treat IDs as discoverable state, not constants for mutation. Resolve the
current library and item IDs by name and exact path before making API calls.

### Tooling

Use existing tools first. Prefer the skill inventory script (no extra deps):

```bash
# On Andromeda — status + candidates report
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --format markdown -o /tmp/qbt-candidates.md

# NUL list of transfer-ready relative names for rsync --from0
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --transfer-ready-only --format nul > /tmp/andromeda-audiobooks.list0
```

In the dotfiles checkout before activation:

```bash
python3 home/config/llm/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --format markdown
```

Run missing one-off tools ephemerally through Nix:

```bash
nix shell nixpkgs#sqlite -c sqlite3 --version
nix shell nixpkgs#ffmpeg -c ffprobe -version
```

Do not persist packages in the dotfiles merely to perform an import.

## New library setup

Use this when creating a library in Audiobookshelf or when adding a new media
root on Terminus.

1. **Filesystem root** — ensure the directory exists under `/srv/media/` with
   ownership `william:users` mode `0755`. Preferred names already managed by
   NixOS tmpfiles: `audiobooks`, `audiobooks-children`, `audiobooks-celine`,
   `phone-audiobooks`. For a brand-new named library, add a matching tmpfiles
   rule in `system/terminus/configuration.nix` (or create the dir once by hand
   and document it), then `mkdir` on Terminus.
2. **Default** — if the user does not specify, configure **Audiobooks William**
   at `/srv/media/audiobooks`.
3. **ABS library** — in the web UI (or API): type **Books**, display name clear
   (e.g. `Audiobooks William`), folder path = the absolute media root. One
   primary folder per library; do not attach the same path to two libraries.
4. **Discover ID** — `GET /api/libraries` and match by name/path. Do not hard-code
   IDs into automation.
5. **Ingest** — leave the tree empty or fully prepared before the first scan.
   Prefer the watcher for new drops; use an explicit scan only when no watcher
   scan is active (see best-practices guide).
6. **Permissions** — `audiobookshelf` is in group `users` so it can read
   `0755` trees owned by `william`. Do not chmod library roots to exclude the
   service user.
7. **Smoke** — drop a single known-good test book into a staging→prepare→move
   path, confirm one item appears, then remove the test if it was synthetic.

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

## Phase 0: qBittorrent download status

Before copying anything from Andromeda, establish **transfer readiness**. An
active or incomplete torrent must not be staged as if it were final.

On Andromeda (or via SSH from another host):

```bash
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --format markdown
```

Interpret statuses:

| Status | Meaning | Copy? |
|---|---|---|
| `complete` | `finished_time > 0` (or equivalent) and path exists | Yes — candidate |
| `incomplete` | Still downloading / unfinished pieces | **No** — wait |
| `missing_path` | Fast-resume name no longer on disk | **No** — skip / re-check |
| `unknown` | Ambiguous progress | Investigate before copy |

Also note:

- qBittorrent may still be **seeding** after completion; that is fine to copy
  if status is `complete` and a checksum dry-run later matches.
- Files can change during a long transfer even when complete if the user
  re-downloads or replaces content — always re-rsync + checksum dry-run.
- Do not use the qBittorrent WebUI password or print credentials; fast-resume
  inventory is enough for status and scope.

Optional: if the GUI is open, confirm the same set of names and progress, but
fast-resume remains the automation source of truth.

## Phase 1: inventory the source

The source of truth for **torrent** download scope is qBittorrent's fast-resume
data, not a recursive copy of `~/Downloads`. The latter can contain unrelated
or private files (PDFs, archives, non-audiobook media).

Use the skill script:

```bash
# Markdown report (candidates / incomplete / missing)
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --format markdown -o /tmp/qbt-candidates.md

# NUL-delimited relative names for transfer-ready only
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --transfer-ready-only --format nul > /tmp/andromeda-audiobooks.list0
```

The inventory helper:

1. reads `~/.local/share/qBittorrent/BT_backup/*.fastresume`;
2. decodes `name` / `qBt-savePath` with a built-in bencode decoder;
3. rejects absolute paths and `..` traversal;
4. joins under the torrent save path (typically `/home/william/Downloads`);
5. classifies `complete` / `incomplete` / `missing_path`;
6. emits existing relative names as NUL for rsync, and reports absent payloads
   separately.

Record an initial manifest with relative path, status, size, and modification
time. Add checksums either before the copy or during the checksum verification
pass.

When working **from Terminus**, pull the list over SSH:

```bash
ssh andromeda \
  'python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
     --transfer-ready-only --format nul' \
  > /tmp/andromeda-audiobooks.list0
```

(After `hmswitch` on Andromeda so the skill script is deployed under
`~/.grok/skills/...`. From a checkout, point at the repo path instead.)

## Phase 1b: Libation source (Andromeda)

Libation is the **Audible liberator** installed on Andromeda (`libation` package).
It is a first-class parallel source arm: same staging, review, import, and
metadata phases as qBittorrent.

### Layout (observed)

| Item | Path |
|---|---|
| Liberated books directory | `/home/william/Music/Libation/Books` |
| App config + logs + DB | `/home/william/.local/share/Libation/` |
| Books path setting | `Settings.json` → `"Books"` |
| Per-title file index | `FileLocationsV2.json` → `Dictionary` keyed by ASIN |
| Library DB | `LibationContext.db` |
| Accounts (do not copy) | `AccountsSettings.json` (identity tokens) |

Typical liberated book: one folder named like
`Title [ASIN]/` containing a single `.m4b` plus optional `.cue` / cover
sidecars. Settings may include `StripAudibleBrandAudio` and high download
quality (e.g. Spatial) — do not re-encode away from the liberated files unless
the user asks.

### Inventory Libation

1. List leaf book directories under `Music/Libation/Books` that still exist.
2. Optionally cross-check `FileLocationsV2.json`: many historical ASINs may
   point at paths that were already moved into Terminus libraries (missing on
   disk). Only paths that **exist** are transfer candidates.
3. Never copy `AccountsSettings.json`, identity tokens, or the full Libation
   config tree to Terminus. Media folders only.
4. Classify by audience: William vs Children vs (explicit only) Celine — same
   AC6 rules. Libation accounts may include more than one Audible profile;
   destination library is about **content audience**, not which account ripped
   the file, unless the user says otherwise.

Example inventory:

```bash
# Present liberated folders
find /home/william/Music/Libation/Books -mindepth 1 -maxdepth 1 -type d -print

# Paths still on disk according to FileLocations (no secrets printed)
python3 - <<'PY'
import json, pathlib
fl = json.loads(pathlib.Path.home().joinpath(
    ".local/share/Libation/FileLocationsV2.json").read_text())
d = fl.get("Dictionary", {})
for asin, entries in d.items():
    for e in entries:
        p = (e.get("Path") or {})
        path = p.get("Path") if isinstance(p, dict) else p
        if path and pathlib.Path(path).exists():
            print(path)
PY
```

If `Books` is empty and all FileLocations paths are missing, there is nothing
new to import from Libation until the user liberates more titles.

### Transfer Libation → Terminus staging

Stage under a dated imports dir, **not** into watched roots:

```bash
# On Terminus
mkdir -p /srv/media/imports/andromeda-libation-YYYY-MM-DD

rsync -a --info=progress2 \
  andromeda:/home/william/Music/Libation/Books/ \
  /srv/media/imports/andromeda-libation-YYYY-MM-DD/
```

Prefer an explicit include list when only a subset is new. Then the same
incremental rsync + checksum dry-run rules as Phase 2.

## Phase 1c: source → target duplicate preflight

**Required** for every source arm (qBittorrent and Libation), before the first
copy and again before any atomic move into a live library root. Skipping this
step wastes transfer bandwidth and risks double catalogue entries.

### What to compare

| Source | Target |
|---|---|
| Transfer-ready qBittorrent names under `~/Downloads` | All live roots: `/srv/media/audiobooks`, `audiobooks-children`, `audiobooks-celine`, `phone-audiobooks` |
| Libation book folders / ASINs under `Music/Libation/Books` | Same live roots |
| Staged payloads under `/srv/media/imports/...` (second pass) | Same live roots + ABS items |

Also scan recent staging trees under `/srv/media/imports/` when deciding
whether a payload was already reviewed/rejected (e.g. prior incomplete course).

### Match signals (cheap → expensive)

1. **Folder / payload name** — exact basename or leaf folder.
2. **Normalized title** — lowercase, strip `[ASIN]`, quality tags, punctuation.
3. **ASIN / ISBN** — from Libation folder names, tags, or `FileLocationsV2`.
4. **Size** — same total bytes → candidate for checksum.
5. **SHA-256** (or strong checksum) — exact file duplicate when sizes match.
6. **Work-level** — same title+author+narrator/edition without byte identity
   (AC5 judgement; still report as duplicate candidate).

### Helper script

On Terminus (or any host that can see the library roots), with a list of source
names:

```bash
# From Andromeda inventory names:
python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
  --transfer-ready-only --format names \
  > /tmp/source-names.txt

# On Terminus — check those names against all library roots:
python3 ~/.grok/skills/audiobook-library-import/scripts/source_target_duplicate_check.py \
  --sources-file /tmp/source-names.txt \
  --targets /srv/media/audiobooks \
            /srv/media/audiobooks-children \
            /srv/media/audiobooks-celine \
            /srv/media/phone-audiobooks \
  --format markdown -o /tmp/source-target-dupes.md
```

Cross-host one-liner (run from a machine with SSH to both):

```bash
ssh andromeda \
  'python3 ~/.grok/skills/audiobook-library-import/scripts/qbittorrent_inventory.py \
     --transfer-ready-only --format names' \
  | ssh terminus \
  'python3 ~/.grok/skills/audiobook-library-import/scripts/source_target_duplicate_check.py \
     --sources-file - \
     --targets /srv/media/audiobooks /srv/media/audiobooks-children \
               /srv/media/audiobooks-celine /srv/media/phone-audiobooks \
     --format markdown'
```

For Libation, feed folder basenames under `Music/Libation/Books` (or ASINs)
as the sources list.

### Disposition

| Preflight result | Action |
|---|---|
| Exact name or ASIN hit on a live path | Default **`duplicate`** — do not copy unless user wants a labelled alternate edition |
| Same-size + matching checksum | **`duplicate`** — skip; never delete the live copy |
| Normalized title hit only | **Investigate** (narrator/edition) before accept |
| No hit | Eligible for Phase 2 staging (still run full AC5 after review) |
| Hit only under `/srv/media/imports/...` | Read prior review notes; may be prior reject — do not auto-import |

Record every match in the accounting table (Phase 3) even when the payload is
never staged. Re-run preflight after staging if the first pass was name-only
and you now have local files for size/checksum comparison.

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

This criterion is the deep review for what Phase 1c preflight already started.
Search all relevant live library roots and Audiobookshelf before importing
(and before treating a staged payload as accepted):

- Phase 1c source→target report (name / ASIN / size hits);
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
Full watcher/scan and layout rules: `audiobookshelf-best-practices.md`.

### API authentication

On **Terminus**, obtain the root token without printing it:

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

## Phase 5b: review, explain, and fix broken metadata

Use this playbook when the user asks to review or fix Audiobookshelf metadata
(wrong title/author/narrator, missing cover, zero duration, bad series, wrong
library placement, tag leakage such as `Plan B` as author).

### 1. Review (discover)

```bash
# Libraries
curl -fsS -H "Authorization: Bearer $token" \
  "http://127.0.0.1:13378/api/libraries" | jq .

# Items in a library (paginate as needed)
lib_id="<resolved-library-id>"
curl -fsS -H "Authorization: Bearer $token" \
  "http://127.0.0.1:13378/api/libraries/${lib_id}/items?limit=50&page=0" | jq .
```

Flag items where any of these hold:

- missing or placeholder title/author;
- narrator/cast empty when the edition needs distinction;
- cover missing;
- duration zero or item marked invalid/missing;
- path does not match the on-disk folder name;
- obvious tag pollution (encoder name, torrent site, `Plan B`, etc.);
- series/sequence wrong or colliding with another edition.

Read-only SQL can help bulk-find suspects; still mutate only via API:

```bash
nix shell nixpkgs#sqlite -c sqlite3 -readonly "$db" \
  "SELECT id, title, path FROM libraryItems WHERE title IS NULL OR title='' LIMIT 20;"
```

(Table/column names can vary by ABS version — inspect schema first with
`.schema` and adapt.)

### 2. Explain

For each suspect item, write a short diagnosis before changing anything:

| Field | Current | Evidence | Proposed |
|---|---|---|---|
| path | … | filesystem | (unchanged) |
| title | … | spoken credits / cover / ASIN | … |
| author | … | … | … |
| narrator | … | … | … |

Do not apply a fix you cannot justify from evidence. Prefer leaving a field
empty over guessing.

### 3. Fix (API only)

1. Resolve **current** item ID by **exact path** immediately before the write.
2. Prefer the ABS item update endpoint for the installed version, e.g.
   `PATCH`/`POST` on `/api/items/<id>/media` or the version-documented update
   route — confirm with a prior GET of the same item and ABS docs for that
   release. Send only fields you intend to change.
3. Never `UPDATE` library item rows in SQLite.
4. Never reuse IDs from an old spreadsheet without re-resolving path → ID.

Example shape (adapt field names to the live API response):

```bash
item_id="$(
  # resolve by exact path via API search or read-only SQL → single ID
  echo "$resolved_id"
)"
curl -fsS -X PATCH \
  -H "Authorization: Bearer $token" \
  -H "Content-Type: application/json" \
  -d '{"metadata":{"title":"Correct Title","author":"Correct Author"}}' \
  "http://127.0.0.1:13378/api/items/${item_id}/media"
```

If the installed ABS version uses a different path or body schema, GET the item
first and mirror its structure; do not invent fields.

### 4. Re-verify

```bash
curl -fsS -H "Authorization: Bearer $token" \
  "http://127.0.0.1:13378/api/items/${item_id}" | jq .
```

Assert:

- `path` unchanged;
- title/author/narrator/cover match the proposal;
- no second item shares the same path;
- listening progress still attached if it existed before.

Report every change and every residual uncertainty in the handoff.

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
- Do not copy incomplete torrents; check status (Phase 0) first.
- Do not skip Phase 1c source→target duplicate preflight before copy or import.
- Do not assume the remote source is static during a long transfer.
- Do not use filename, bitrate, tags, or `ffprobe` alone as acceptance evidence.
- Do not import into a watched root before the book layout is complete.
- Do not run watcher ingestion and explicit scans together.
- Do not mutate Audiobookshelf's SQLite database directly.
- Do not bulk-apply metadata using positional or stale item IDs.
- Do not delete a duplicate database item with progress or sessions.
- Do not clean staging until final checksum and database accounting pass.
- Do not copy Libation `AccountsSettings.json` or identity tokens to Terminus.
- Do not treat empty `Music/Libation/Books` plus missing FileLocations paths as
  failures — usually those titles were already imported.
- Do not put new imports into Celine or Phone without an explicit request;
  default is William (`/srv/media/audiobooks`).
