# Daily workflow

Search the launcher for **Pi workspace**, **Today's notes**, **Today's agenda** or
**System cleanup**. Each opens a fresh terminal. Pi workspace runs the existing
`mux start dot` with the herdr backend: mux focuses the existing `dot` workspace
or creates it, then attaches. It does not start a separate Pi process or replace
workspace contents. Notes runs the existing Fish `today` function.

Cleanup requires typing `DELETE` before calling `~/.bin/gcall`. That command
deletes old user and system Nix generations, removing rollback options. Merely
opening the launcher or the confirmation terminal does not collect garbage.

## Agenda

`daily-agenda` displays today's notes with cached events from named private Google
Calendar iCal feeds, merged by time and labelled by calendar. The launcher reads
cached events immediately, without waiting for the network, and shows the last
refresh time. The background timer refreshes every fifteen minutes.
`daily-agenda --refresh` fetches all feeds on demand. A successful refresh also writes the rail calendar's read-only events file at `~/.local/state/omarchy/calendar-events.json`. That write does not change Google Calendar. If the export fails, the previous events file stays in place. `daily-agenda --status`
reports configuration and cache age without printing secret URLs.

The notes source is `~/Notes/YYYY-MM-DD/today.md`. Unfinished checkboxes and bullets
under Reminders or What I plan to do today are shown; completed checkboxes are
excluded. Notes and calendar are never modified. Missing access and stale cache
are shown explicitly, rather than being reported as an empty calendar.

The feed is read for the local civil day, including a 23- or 25-hour daylight-saving
transition. Recurring events include overrides, exclusions and cancellations.
All-day events use an exclusive end date, so an event ending on a date does not
occupy that date. Timed events are shown in local time. Floating times use the
calendar timezone when the feed supplies one, otherwise the local timezone.

A refresh that is too large, too slow, redirected, or malformed does not replace
the previous cache. The command carries its own calendar parser; do not run
`agenda.py` with another Python.

Graphical Linux hosts refresh every fifteen minutes when the feed file exists.
Other hosts can run the portable CLI manually. The cache lives under
`$XDG_CACHE_HOME/daily-agenda` (normally `~/.cache/daily-agenda`), with private
permissions. It contains personal event details: do not commit or share it.

## Connect Google Calendar

Use the calendar's secret iCal address, not an OAuth client and not a Gmail app
password. In Google Calendar, open **Settings for my calendars**, select the
calendar, then **Integrate calendar**, and copy **Secret address in iCal format**.
Do not use the public address.

The value must be one line of the form
`https://calendar.google.com/calendar/ical/<calendar-id>/private-<secret>/basic.ics`.
Only that HTTPS host and private `basic.ics` path are accepted. Redirects are not
followed, and the address is not written to the cache, command output or journal.

Store a JSON object mapping calendar labels to their private URLs as the SOPS
value `GOOGLE_CALENDAR_ICAL` in the private `nix-config` repository. The original
single-URL format is still supported. If any feed fails, keep the previous complete
cache rather than silently showing an incomplete agenda. A calendar shared only
with viewing permission requires its owner to provide the private feed address. Never put plaintext in this repository, Nix expressions,
command arguments, terminal output or chat. Add its declaration to an appropriate
secret group and select that group on each authorised host. Follow that
repository's edit/deploy procedure, update the pinned input here, build and
activate.

The Home Manager wrapper uses the declared sops-nix secret path when available,
otherwise the standard Home Manager secret directory. `DAILY_CALENDAR_CREDENTIALS`
can point to a private feed file for setup/testing. This does not change the
background timer's configured path. The file must be a regular file with no group
or world permissions; mode `0400` or `0600` is suitable.

SOPS distributes the address, not a substitute for possession of it. Each host
still needs permission to decrypt the private input and direct network access to
`calendar.google.com`. Revoke a leaked address by resetting the calendar's secret
address in Google and replacing the SOPS value.

After activation, run `daily-agenda --status`, then `daily-agenda --refresh` in a
private terminal. Verify a known event and today's note reminders. On graphical
Linux, inspect `systemctl --user status daily-agenda-refresh.timer`. Refresh
service output is discarded to keep event details and the feed address out of
the journal.
