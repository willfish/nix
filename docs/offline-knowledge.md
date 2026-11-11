# Offline knowledge library

Terminus stores the canonical library at `/srv/media/knowledge` on `tank/media`.
The original documents and search indexes remain useful without an AI model.
Pi and OpenCode can use the existing models on Andromeda or Relay to search,
read passages and explain them with citations. This deployment changes no model,
inference engine or provider settings. See [local inference](local-llm.md) for
existing provider selection.

## Runtime and clients

`programs/book-library` owns the CLI, read-only MCP implementation and explicit
maintenance commands. `home/user/book-library.nix` installs the runtime and an
opt-in Terminus user job. `home/user/llm-mcps.nix` is the shared registration source
for Pi and OpenCode V2; do not edit generated client JSON files.

| Connection | Transport |
| --- | --- |
| `books` on Terminus | Local stdio process and local SQLite |
| `books` on other homes | Verified native-LAN SSH to `william@terminus.fritz.box` |
| `books-local` on Linux desktops/laptops | Local essentials copy, with no SSH attempt |

Activate an affected Linux home with `hmswitch`, then refresh the client's MCP
catalogue. OpenCode watches configuration; `/reload` or reconnect if needed.
For Pi, use `/reload` or a new session and connect `books` through `/mcp`.
Registration alone does not prove source coverage or a host's connectivity.
Use an existing provider that is reachable from the client. A loaded loopback-only
model is not automatically exposed to other machines; this deployment neither
opens model ports nor starts another inference service.

## Explicit acquisition

Source policy selects practical/reference content first, with recreation separate.
Discovery freezes a specific acquisition manifest, not an endlessly changing
remote catalogue. Inspect `selection-report.json` and the source manifest before
enabling acquisition. Metadata-only records are not downloaded books.

Archives must have native full-text indexes. The current iFixit archive does not;
its exclusion is recorded, with the indexed DIY and mechanics collections serving
repair questions instead. Do not treat an excluded archive as acquired coverage.

Run on Terminus as `william`, without sudo:

```sh
book-library lock
book-library acquire --limit 5
book-library ingest --limit 5
book-library status
```

For a metadata-only update comparison, use a new staging directory on ZFS:

```sh
book-library check-updates /srv/media/knowledge/update-candidate
```

This resolves source metadata without downloading originals or replacing the live
manifest. Never edit a frozen lock to disguise missing or changed content.

After inspecting representative results and the selected footprint:

```sh
touch /srv/media/knowledge/acquisition-enabled
systemctl --user start book-library-maintain.service --no-block
systemctl --user status book-library-maintain.service
journalctl --user -u book-library-maintain -n 40 --no-pager
```

The job handles bounded download/index batches at low priority. It does not start
another download until the preceding original has been indexed, so smaller
references become usable while larger transfers remain pending. It does not start
from a Home Manager activation. Remove only the enable marker to finish the
current batch and pause; stopping the unit retains committed checkpoints. Do not
restart a healthy worker merely to inspect progress. Provider cooldowns remain
in effect after a restart.
Untried sources precede retries; failed sources rotate by oldest attempt across
restarts. A retained failure remains incomplete and subject to the same provider
cooldown, rather than blocking every untried work from that provider.

OAPEN downloads use the PDF filename from its catalogue, without an invented
bitstream sequence. Older frozen URLs retain `sequence=1` for stable source
identities, but acquisition omits that parameter from the request. The partial
receipt records the actual request and final response URLs; a partial belonging
to a different request URL is restarted, not appended. Do not guess sequence
numbers or accept extracted text in place of the preserved PDF.
OAPEN acquisition attempts are separated by at least ten seconds, including failed attempts,
in accordance with its published crawl delay. Provider cooldowns take precedence.
Any unsuccessful HTTP response retains `partial/SOURCE_ID.refusal.json` with the
request, status, any cooldown deadline and bounded response headers. Error bodies,
cookies and authentication headers are not retained. Inspect this receipt when
diagnosing repeated refusals; it never authorises an early retry or another route.
`Retry-After` seconds or standard HTTP dates are honoured on other errors too,
including HTTP 500. Without that header, HTTP 500 does not invent a host cooldown;
the existing default pauses for HTTP 403, 429 and 503 remain in effect.

During explicitly authorised autonomous acquisition, `book-library-supervise.timer`
recovers an accidentally stopped maintenance job. It respects both removal of
`acquisition-enabled` and an `acquisition-hold` file, and never shortens provider
cooldowns. To deliberately pause, remove the enable marker or create the hold
file before stopping maintenance. The supervisor is not started by Home Manager
activation and stops itself after the pinned full-corpus audit passes.

All bulk data and scratch stay on ZFS. The application enforces a 1 TiB incremental
cap and a 512 GiB / 20% free-space reserve. Do not enlarge the cap, move data to the
OS disk, delete other media or change pool layout to bypass a capacity failure.

After acquisition and ingestion have completed:

```sh
book-library audit
```

This independently checks selected originals, hashes, searchable states and
SQLite/FTS integrity. `--quick` omits rehashing and is not a full hash audit.

## Portable copies and originals

`book-library snapshot NEW_DESTINATION` makes a consistent essentials copy from
searchable sources. The destination must be new; it never overwrites a working
copy. Audit a published copy before selecting it through `books-local`. The
portable runtime and knowledge must fit the laptop's separate 64 GiB budget; the
content command reserves room by limiting its content budget to 56 GiB. No laptop
model or inference engine is installed. A portable subset is not a backup of the
full library.

The snapshot includes `INDEX.tsv`, preserved originals and `text/*.txt` exports
for books. The exports include source locations and licence notices and can be
read without SQLite or this application. ZIMs remain compressed and can be opened
with Kiwix. Copy the complete snapshot, not just `library.sqlite3` from a live job.

On Foundation, copy a verified snapshot to a new directory, audit it, then select
it with the `~/.local/share/book-library/essentials` symlink. Keep the preceding
verified directory until its replacement has passed. `books-local` always uses
that explicit local selection and never falls back to SSH mid-conversation.

For example, on Terminus, using a new release directory:

```sh
book-library snapshot /srv/media/knowledge/essentials/release-1 --max-gib 56
book-library --root /srv/media/knowledge/essentials/release-1 audit
rsync -a /srv/media/knowledge/essentials/release-1/ \
  william@foundation.fritz.box:.local/share/book-library/release-1/
```

Create the destination parent on Foundation first. After copying, on Foundation:

```sh
book-library --root "$HOME/.local/share/book-library/release-1" audit
ln -s "$HOME/.local/share/book-library/release-1" \
  "$HOME/.local/share/book-library/essentials"
book-library --root "$HOME/.local/share/book-library/essentials" search 'water'
```

The initial `ln` must fail rather than overwrite an existing selection. For an
upgrade, create a new symlink beside the current one and rename it over the old
symlink only after the replacement's audit succeeds. Never replace a real
directory with a deployment link.

Use terminal search/read if no model is running. Export a selected original to a
new file and open it in a native PDF/EPUB reader to inspect diagrams or tables.
ZIM files can also be opened by Kiwix independently of this application.

## Recovery and separate copies

Keep an exported retrieval-runtime Nix closure, its store-path list and a source
archive under `/srv/media/knowledge/recovery`. Keep a GC root for the installed
runtime. Neither a flake lock nor Terminus's expiring binary cache is a recovery
bundle. Exclude SSH, SOPS and API credentials from archives and copy those only
through the existing encrypted secret-management procedure.

On a compatible NixOS machine, import the retained runtime closure and invoke the
recorded `book-library` store path. Copy the knowledge snapshot to a new directory,
run `book-library --root NEW_DIRECTORY audit`, and select it only after success.
Restoring a library never requires overwriting the live directory.

For an attached backup mounted at `/mnt/knowledge-backup`, for example:

```sh
nix-store --import < /mnt/knowledge-backup/recovery/book-library-runtime.nar
runtime=$(cat /mnt/knowledge-backup/recovery/runtime-store-path)
test ! -e "$HOME/.local/share/book-library/restore-candidate"
mkdir -p "$HOME/.local/share/book-library/restore-candidate"
rsync -a /mnt/knowledge-backup/essentials/ \
  "$HOME/.local/share/book-library/restore-candidate/"
"$runtime/bin/book-library" --root "$HOME/.local/share/book-library/restore-candidate" audit
```

Use the backup's actual essentials directory and the recorded compatible runtime
architecture. A successful import alone does not verify the reference files.

The full corpus needs a separate destination large enough for its originals and
indexes. With maintenance paused, copy the entire root to an authorised external
destination, then audit that copy. Do not call a same-pool recovery directory a
backup, or Foundation's essentials copy a backup of the full library. No external
full-corpus backup is implied by this deployment.

For limited power, prioritise Foundation's reference copy first, then LAN
equipment and Terminus, then an existing inference host when available. These are
priorities, not measured battery or off-grid runtime promises.

## Privilege boundary and operator handoff

Corpus acquisition, indexing, SSH/stdio MCP and Linux Home Manager deployment do
not require sudo. Existing `/srv/media` ownership and user lingering must be
checked before starting. No new dataset, firewall exception, service account or
blanket sudo permission is required by this design.

Any administrator action belongs at the final operator handoff, after the
non-root work. If lingering is actually disabled, the operator can enable it on
Terminus. Do not run speculative NixOS rebuilds or recursive permission changes.
Headless Mac activation can require a separate privileged paired deployment;
do not invoke it as a workaround for this boundary.

Physical WAN-disconnection, cold-start and shared-service interruption tests need
an agreed maintenance window. Unexecuted tests must remain recorded as gaps.
Indefinite availability still depends on power, intact hardware and independent
maintained copies, not simply on having a large model or a ZFS pool.
