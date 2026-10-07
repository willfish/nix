# Slack session refresh

Native replacement for the embedded Python refresh helper. It reads Slack cookies
through the configured Brave CDP endpoint, reuses a matching workspace tab or
creates one background tab, and closes only the tab it created. It never navigates
an existing tab or launches a browser. The low-level CDP connection is shared with
the AWS portal service; no account/login logic is shared.

`auth.test` must succeed before an atomic, private replacement of the literal
`SLACK_COOKIE_D` and `SLACK_XOXC` assignments. Failed authentication preserves the
previous file. Optional SOPS updates use JSON values on stdin, never argv, and do
not commit or push the private repository. Credential values and remote error
bodies are not printed.

The existing environment controls remain: `SLACK_CDP_URL`, `SLACK_TEAM_ID`,
`OUT_FILE`, `XDG_CONFIG_HOME`, `SLACK_UPDATE_SOPS`, `NIX_CONFIG_ROOT` and
`SLACK_DOTFILES_ROOT`. `--help` has no browser or credential side effects.

Run the program-local checks manually with
`cargo test --locked --manifest-path programs/slack-session/Cargo.toml`.
They use fake browser/auth responses, disposable files and a stub SOPS command.
They are not installed or registered as package/flake/hook checks. Real refreshes
and encrypted-secret updates require the user's authorization.
