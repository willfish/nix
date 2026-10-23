# Standup recap

`standup-recap` fills the "What I did yesterday" section of
`~/Notes/YYYY-MM-DD/today.md` with the previous working day's activity. Monday
looks back to Friday; weekend runs exit without doing anything unless `--force`
is given.

A Pi print-mode run gathers GitHub, Slack, Outlook mail and Outlook calendar
meetings through the configured MCP servers and prints bullet lines between
`<<<STANDUP-RECAP` and `STANDUP-RECAP>>>`. Pi never edits the note. This program
checks the block (only `- ` lines, at most 25, each under 400 bytes, no control
characters) and replaces the section atomically. It creates the note from
`templates/today.md` when it is missing. A section that already has entries,
before or after the Pi run, is left unchanged.

## Configuration

`~/.config/standup-recap/config` (`key = value`, `~/` expands to `$HOME`) sets
`provider`, `model`, `thinking`, `timeout_seconds` (30-7200), `notes_dir`,
`pi_command` and `prompt_file`. The prompt defaults to `prompt.md` beside the
config and may use `{{TODAY}}`, `{{PREVIOUS_DAY}}`, `{{PREVIOUS_DAY_NAME}}`,
`{{BEGIN}}` and `{{END}}`. Both files come from `home/config/standup-recap/`.

The read-only boundary for Slack, Outlook and GitHub is set by the prompt, not by
tool restrictions: the MCP gateway can still reach write tools. Outlook needs a
signed-in Brave session; the prompt allows reopening the Outlook tab once but
never entering credentials. An unavailable source is skipped and named on Pi's
final `Unavailable:` line in the log.

## Run state

Each day has `~/.local/share/standup-recap/cache/DD/MM/YYYY/` holding:

- `started`, touched when an attempt begins;
- `completed`, touched when the note was updated or already filled;
- `failed`, touched when an attempt failed (config, note, Pi exit, timeout or
  invalid output);
- `today.log`, timestamped steps, Pi's stderr and Pi's final output.

A later attempt clears `failed`. A day with `completed` is skipped unless
`--force` is given, so the login and 09:00 timer triggers run at most once
successfully. Only the five newest day directories are kept.

## Deployment

`home/user/daily-workflow.nix` installs the wrapper, the configuration and a
oneshot user service with a timer (`OnStartupSec=3m`, weekdays at 09:00,
`Persistent=true`) on graphical hosts with private configuration.

```sh
systemctl --user start standup-recap.service
journalctl --user -u standup-recap.service
```

## Manual check

Point `XDG_CONFIG_HOME` and `XDG_DATA_HOME` at a disposable directory with a
config whose `notes_dir` is disposable and whose `pi_command` is a stub script
that prints a marker block. Run the built binary with `--force`.
