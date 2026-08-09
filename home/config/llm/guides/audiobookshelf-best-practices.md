# Audiobookshelf best practices (Terminus)

Operational guide for William's Audiobookshelf (**ABS**) deployment on
**Terminus**. Pair with `audiobook-library-import.md` for Andromeda → Terminus
acquisition and import. This document is about how the server, libraries, and
API should be used day to day.

## Contents

- [Deployment facts](#deployment-facts)
- [Libraries and roots](#libraries-and-roots)
- [One-book-per-folder layout](#one-book-per-folder-layout)
- [Watcher vs explicit scan](#watcher-vs-explicit-scan)
- [Staging vs live roots](#staging-vs-live-roots)
- [Authentication and secrets](#authentication-and-secrets)
- [API mutation discipline](#api-mutation-discipline)
- [Duplicates and item safety](#duplicates-and-item-safety)
- [Metadata quality](#metadata-quality)
- [Covers, chapters, and matching](#covers-chapters-and-matching)
- [Backups and data loss boundaries](#backups-and-data-loss-boundaries)
- [Health checks](#health-checks)
- [Anti-patterns](#anti-patterns)

## Deployment facts

| Fact | Value |
|---|---|
| Host | **Terminus** (NixOS, ZFS `tank/media` → `/srv/media`) |
| Service | `services.audiobookshelf` in `system/terminus/configuration.nix` |
| Bind | `0.0.0.0:13378` (official default; LAN + Tailscale) |
| Service user | `audiobookshelf`, extra group `users` (read shared media) |
| Config / DB | `/var/lib/audiobookshelf/config/` (includes `absdatabase.sqlite`) |
| Media pool | `/srv/media` (not under the service state dir) |

Clients: web UI, iOS app, LAN or Tailscale to Terminus:13378.

Do not relocate library media into `/var/lib/audiobookshelf` — keep media on
`/srv/media` so ZFS datasets and backups stay clear.

## Libraries and roots

| Library (typical name) | Filesystem root | Default for new imports? |
|---|---|---|
| Audiobooks William | `/srv/media/audiobooks` | **Yes** |
| Children | `/srv/media/audiobooks-children` | Only for YA/children content |
| Celine | `/srv/media/audiobooks-celine` | Explicit only |
| Phone | `/srv/media/phone-audiobooks` | Explicit only |

Roots are created by systemd-tmpfiles as `william:users` `0755`. ABS reads them
via group `users`.

Rules:

1. **One library ↔ one primary folder path.** Do not point two libraries at the
   same directory (duplicate items and confusing scan behaviour).
2. **Default new library / default import destination = William** unless the
   user names another.
3. Library IDs in the database/API are **discoverable state**. Resolve by name
   or folder path before automation; do not treat old UUIDs as permanent
   constants across reinstalls.
4. Creating a new library: ensure the directory exists → create Books library in
   UI/API → attach folder → controlled first scan. Details in the import guide
   “New library setup”.

## One-book-per-folder layout

ABS discovers books from folder structure. Preferred patterns:

```text
/srv/media/audiobooks/
  Author Name/
    Series Name/                    # optional
      Book Title/
        Book Title.m4b              # or track001.mp3 …
        cover.jpg                   # optional but preferred
        metadata.json / desc.txt    # optional sidecars
```

Flat leaf folders also work when already used in the library:

```text
/srv/media/audiobooks/
  Book Title [ASIN]/
    Book Title [ASIN].m4b
```

Rules:

- **Exactly one book per leaf folder** that ABS treats as an item.
- All tracks for that book stay inside the leaf; do not nest a second book
  under the first.
- Multi-file chapter splits belong to one book folder with consistent naming
  (`01`, `02`, …) and no gaps unless the edition is incomplete (which should
  have been rejected at import).
- Collections that should be **separate catalogue entries** need **separate
  leaf folders** (see Roger Hargreaves precedent in the import guide).
- Prefer preparing the full tree under `/srv/media/imports/...` or a sibling
  prepare dir on the **same filesystem**, then `mv` into the live root so the
  watcher sees a complete folder.

## Watcher vs explicit scan

This deployment expects the **filesystem watcher** to notice new folders under
library roots.

| Mode | When to use |
|---|---|
| Watcher only | Normal imports after atomic moves into the live root |
| Explicit library scan | Recovery, bulk path fix, or when watcher missed items — **only if no watcher scan is already running** |
| Both at once | **Never** — July 2026 race created dozens of duplicate zero-progress DB items |

Procedure for imports:

1. Finish prepare + checksum outside the watched root.
2. Atomic move/rename into the library root.
3. Wait for the watcher to settle (new items appear, no “scanning” thrash).
4. Only if items are missing after a reasonable wait: trigger **one** explicit
   scan for that library, then re-check.

Do not script tight loops of “scan all libraries” during an import batch.

## Staging vs live roots

| Path | Watched by ABS? | Role |
|---|---|---|
| `/srv/media/imports/...` | **No** (must stay that way) | Staging, review, rejects, duplicates |
| `/srv/media/audiobooks*` | **Yes** | Live libraries only |
| `/srv/media/phone-audiobooks` | **Yes** | Phone library |

Never rsync Andromeda downloads straight into a live library root. Never run
long `ffmpeg` rewrites inside a live leaf while the watcher is active; work in
staging/prepare, then replace atomically if needed.

## Authentication and secrets

- Prefer API calls with a user **token** (root or dedicated automation user).
- Discover token read-only from SQLite when operating on Terminus:

  ```bash
  db=/var/lib/audiobookshelf/config/absdatabase.sqlite
  token="$(
    nix shell nixpkgs#sqlite -c sqlite3 -readonly "$db" \
      "SELECT token FROM users WHERE username='root';"
  )"
  ```

- Pass as `Authorization: Bearer $token` only.
- **Never** print tokens, commit them, put them in reports, or enable `set -x`
  around token assignment.
- Do not expose the admin password in chat logs.
- Remote clients should use Tailscale/LAN; do not casually open ABS to the
  public internet without auth review.

## API mutation discipline

Iron rules (also in the import skill):

1. **Resolve item ID by exact filesystem path** immediately before each write.
2. Require **exactly one** matching item; stop if zero or many.
3. Capture path, title, and progress/sessions before mutate.
4. Use **HTTP API** for updates and item deletes — not `UPDATE`/`DELETE` on SQLite
   media tables.
5. **Re-GET** the item by ID after every write; assert path unchanged and
   metadata matches intent.
6. Prefer minimal patches (only fields you mean to change).

SQLite is allowed for:

- token discovery;
- read-only investigation and accounting counts;
- finding suspect rows to then fix via API.

SQLite is **not** allowed for:

- fixing titles/authors in place;
- deleting items without API;
- “quick” bulk SQL updates from a spreadsheet of IDs.

## Duplicates and item safety

| Situation | Action |
|---|---|
| Same path, two DB items | Scanner race — delete the **extra DB item** via API after confirming zero progress; keep media once |
| Same checksum, second copy incoming | Skip import; do not delete the existing library file |
| Same work, different narrator | Keep both only if labelled and intentional |
| Item has listening progress | Never delete casually; never “replace” by deleting the progressed item |
| `DELETE /api/items/<id>` | Removes library **record** — confirm whether media delete is requested separately; default **keep files** unless user asks |

Always distinguish **duplicate media on disk** from **duplicate database rows**.

## Metadata quality

Good metadata for this household:

- Correct **title** and **author** as listeners will search.
- **Narrator** / cast / production type when editions could collide (e.g. Fry
  vs full-cast Harry Potter — never cross-label).
- **Cover** present and appropriate.
- **Duration** positive and plausible vs files.
- **Series** + sequence when it helps browsing.
- ASIN/ISBN stored when reliable (Libation rips often carry ASIN in folder
  names).
- No torrent-site, encoder, or random tag leakage in author/title fields.

Broken metadata workflow: review → explain with evidence → API fix → re-verify
(import guide Phase 5b).

## Covers, chapters, and matching

- Embedded covers in `.m4b` are good; a `cover.jpg` in the leaf folder is a
  reliable fallback for ABS.
- Chapter data: prefer embedded chapters in m4b; cue sheets are supporting
  evidence, not a substitute for a clean single-file book when available.
- Provider matching (Audible/etc. in ABS UI): useful for fills; still verify
  narrator and edition before accepting a match that could overwrite a correct
  manual label.
- After provider match, re-check path binding — matching must not attach the
  wrong edition to a folder.

## Backups and data loss boundaries

- **Media** lives on ZFS `tank/media`. Plan backups/snapshots at that layer.
- **ABS config/DB** lives under `/var/lib/audiobookshelf`. Losing the DB loses
  progress, users, and library config even if files remain.
- Import staging under `/srv/media/imports` can be large; do not delete staging
  until final accounting passes (import guide Phase 6).
- Phone library may contain device-sync copies; treat as a separate scope from
  William.

## Health checks

Quick Terminus checks (read-only):

```bash
systemctl is-active audiobookshelf
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:13378/
ls /srv/media/audiobooks /srv/media/imports
```

With token (do not print it):

```bash
curl -fsS -H "Authorization: Bearer $token" \
  http://127.0.0.1:13378/api/libraries | jq '.[].name'
```

After an import batch: every new path → exactly one item; no new
invalid/zero-duration items; progress on pre-existing items unchanged.

## Anti-patterns

- Copying all of `~/Downloads` into `/srv/media/audiobooks`.
- Importing incomplete qBittorrent payloads.
- Running watcher + manual scan on the same batch.
- Writing metadata with SQL.
- Bulk metadata from a stale ID column.
- Deleting “duplicate” items that still have progress.
- Pointing a second library at William's path “just for testing”.
- Shipping Libation account tokens or ABS tokens in git, chat, or scratch
  reports.
- Using Celine/Phone as a dumping ground for unsorted imports.

## Related

- Skill: `audiobook-library-import` (`/audiobook-library-import`)
- Import runbook: `audiobook-library-import.md`
- Host wiring: `system/terminus/configuration.nix`
- Inventory helper: skill `scripts/qbittorrent_inventory.py`
