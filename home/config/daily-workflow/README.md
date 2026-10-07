# Daily workflow

`daily-workflow workspace|notes|agenda|cleanup` opens a fixed command in Ghostty
through a detached systemd user scope. Notes and agenda retain their window titles
and sizes. Launches remove `HERDR_*` and `MUX_HERDR_ATTACH`, set
`MUX_BACKEND=herdr` and preserve the caller's working directory. Paths and actions
are separate arguments, not launcher query text interpreted by a shell.

`_agenda` and `_cleanup` are internal terminal actions. Children inherit standard
streams and the unchanged environment. Agenda closes on one raw key when stdin is
a terminal, discarding queued input first and restoring saved terminal settings.
With nonterminal stdin, agenda waits for Enter or EOF. Interrupting an internal child wait
kills/reaps that child rather than leaving it running.

Cleanup warns about loss of rollback options and runs `$HOME/.bin/gcall` only after
exact `DELETE` confirmation. Whitespace, case changes, NUL and piped CRLF do not
count as confirmation. EOF or an interrupted prompt cancels without running it.
Cleanup remains in canonical mode and waits for Enter to close. Invalid actions
return 64; failures to start use a bounded diagnostic without disclosing paths.

## Manual fixtures

Fixtures are default-off, noninstalled and not registered as Meson tests or Nix
checks. On Linux:

```sh
direnv exec . nix develop .#daily-workflow -c meson setup \
  /tmp/daily-workflow-build home/config/daily-workflow \
  --buildtype=debugoptimized -Dfixtures=true
direnv exec . nix develop .#daily-workflow -c meson compile \
  -C /tmp/daily-workflow-build
DAILY_WORKFLOW_BIN=/tmp/daily-workflow-build/daily-workflow \
DAILY_WORKFLOW_PTY=/tmp/daily-workflow-build/daily-pty \
  /tmp/daily-workflow-build/daily-checks
```

The driver uses disposable homes, stub calendar/cleanup commands and a compiled
PTY observer. It never runs live garbage collection or opens graphical terminals.
Optional retired-CLI parity uses `DAILY_WORKFLOW_LEGACY` and
`DAILY_WORKFLOW_PYTHON`, neither a package dependency. Sanitizer builds can add
`-Db_sanitize=address,undefined -Db_pie=false -Dc_link_args=-no-pie`.
