# TOTP with encrypted seeds

Before updating this guide, read `~/.agents/guides/documentation-relevance.md`.

Use `totp-sops-import <NAME>` in a local terminal to import a standard Base32
TOTP seed into the private encrypted configuration. Never paste seeds into chat,
command arguments, logs, commits or screenshots. Use the hidden prompt, stdin,
or an existing mode-600 seed file. Do not store generated codes.

Secrets use `TOTP_<NAME>_SECRET`. Imports target the private checkout's
`secrets/env.yaml`; `NIX_CONFIG_ROOT` selects another checkout. Add the name to
the appropriate private secret group, then commit and push the encrypted change,
update the consuming flake input, build and activate Home Manager.

## Safe migration

Re-enrol one account at a time through its security settings. Change an
existing authenticator only with explicit authorization and a working recovery
method. Import the new seed locally, deploy it, and verify with the user that
the generated code matches before removing the old authenticator entry.

`totp-from-sops <NAME>` generates a short-lived code. Omitting the name opens an
interactive account picker; automation must select a name explicitly. Fill the
code through the approved browser workflow without logging it. A missing seed
requires local import or human MFA, not a credential workaround.

Push and number-match challenges require human interaction. Proprietary Authy
tokens cannot be converted to standard TOTP. Do not use unsupported desktop
scrapers or invasive phone/export methods without a separately approved plan.

Check availability without revealing values:

```sh
test -r "${SOPS_NIX_SECRETS_DIR:-$HOME/.config/sops-nix/secrets}/TOTP_EXAMPLE_SECRET"
```
