# Daily agenda

Read-only notes and private Google Calendar feeds, implemented in C with GLib,
libical, libcurl and yyjson. `daily-agenda` displays saved events and reminders
without network requests. `--refresh` updates the daily cache and panel export;
`--status` reports availability without disclosing feed URLs.

## Runtime contract

- `DAILY_CALENDAR_CREDENTIALS` selects a private regular file, at most 4096 bytes.
  Symlinks are permitted only through the target's permission checks. Supply one
  private Google `basic.ics` URL or a JSON object of one to eight named feeds.
  The Home Manager wrapper retains the sops secret default.
- Requests are HTTPS-only, use no environment proxies and never follow redirects.
  TLS verifies the peer and hostname; an explicitly supplied `SSL_CERT_FILE`
  must be usable. Connection and idle timeouts are twenty seconds, not a total
  transfer deadline. Errors never include the feed address or token.
- Expansion runs the actual executable in a separate child with a reduced
  environment, a sixty-second wall deadline, thirty-second CPU limit, 1 GiB
  address/data limits, 2 MB file limit and no core dumps. Calendar input and
  expansion output are bounded to 2 MB; component and occurrence counts remain
  bounded independently.
- Recurrence preserves daily/range overlap, exclusions, moved/cancelled instances,
  `THISANDFUTURE`, nominal duration and timezone behavior. Preserve the previous
  engine's ignored `EXRULE` behavior rather than applying new exclusions.
  IANA data is supplied by the package; local time falls back to `/etc/localtime`.
- `$XDG_CACHE_HOME/daily-agenda/calendar.json` and
  `$XDG_STATE_HOME/omarchy/calendar-events.json` retain their existing schemas.
  Writes use private, fsynced atomic temporaries and refuse a symlink destination
  or immediate parent. A failed refresh retains saved events. A failed panel
  export retains the previous rail cache without undoing a successful daily save.
- Notes come from `$HOME/Notes/YYYY-MM-DD/today.md`. Unfinished checkboxes and
  reminder sections are deduplicated; fenced blocks are ignored. Unicode
  category-C characters are removed from displayed text. Feed URLs and tokens
  are redacted before calendar text is persisted or displayed.

## Manual fixtures

Fixtures are optional, never installed and not registered as Meson tests. Build
with `-Dfixtures=true` in a disposable directory. Override `-Dzoneinfo=...` if
`/etc/zoneinfo` is unavailable outside the Nix package. Run from the repository's
existing direnv environment, with GLib/GIO, yyjson, libcurl, libical, Meson,
Ninja and pkg-config supplied by an ephemeral Nix shell.

```sh
nix develop .#daily-agenda --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/agenda-checks programs/daily-agenda \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#daily-agenda --command meson compile -C /tmp/agenda-checks
/tmp/agenda-checks/agenda-checks
```

`DAILY_AGENDA_BIN`, `DAILY_AGENDA_FIXTURE`, `DAILY_AGENDA_LIMITS`,
`DAILY_AGENDA_PTY` and `DAILY_AGENDA_NETWORK` override the sibling binaries.
The Linux routing fixture still redirects only fixture processes to a disposable
loopback HTTPS server. The check builds that certificate with OpenSSL.

The Linux routing fixture redirects only fixture processes to a disposable
loopback HTTPS server while retaining Google Host/SNI and certificate validation.
It is not part of the production binary. No test reads real credentials or
contacts a private calendar. Set `DAILY_AGENDA_LEGACY` to a retained legacy wrapper
for optional output parity comparisons.

For ASan/UBSan, use a separate sanitized build and a normal compiled child via
`DAILY_AGENDA_CHILD`; the 1 GiB child limit cannot accommodate sanitizer shadow
memory. Set `DAILY_AGENDA_PARENT_FIXTURE` to the sanitized `agenda-fixture` to
exercise refresh with that normal child. Preload the sanitizer runtime before the
network routing fixture. Continue to use normal `agenda-limits` and `agenda-pty`
probes. None of these controls are recognized by the installed command.
