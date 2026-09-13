# Reproducible Hermes on Relay

Relay's Hermes runtime is pinned by the `hermes-agent` flake input. The CLI,
Qwen launcher and system gateway use the same Nix package rather than the
editable checkout and virtual environment under `~/.hermes/hermes-agent`.
The existing checkout is retained for recovery; it is not the managed runtime.
The package uses upstream locked Python and npm dependencies. npm lifecycle
scripts are disabled; the declared UI build commands still run in the sandbox.

Home Manager installs an encrypted private declaration from
`nix-config/secrets/relay-hermes.json`. It contains the main and Qwen settings,
personality files, custom skills, scripts, assets, plugins and scheduled-job
definitions. Files already supplied by other Home Manager modules remain owned
by those modules. SOPS decrypts the declaration at runtime; plaintext settings,
credentials and private assets must never enter Git or the Nix store.

## Configuration versus state

Declared files are restored on activation. A content-addressed backup of each
replaced file is retained under `~/.hermes/backups/home-manager`. Files removed
from a previously installed declaration are backed up and removed. Unrelated
files are not deleted during the initial migration.

Scheduled-job definitions are authoritative. The installer shares Hermes's
scheduler lock, preserves execution bookkeeping for unchanged schedules and
keeps completed jobs disabled. Jobs removed from the declaration are removed
from the scheduler configuration, not from its execution databases. A fresh
home starts without execution history; missing next-run times are computed by
Hermes. Restoring machine history requires a separate state backup.

OAuth files are seeded only when absent, so activation does not roll back
refreshed credentials. The Qwen profile receives the current declared local-model
overlay and local API key before it is written. An existing local API key is
retained. Expired credentials may still require login; reproducible installation
does not make external grants permanent.

Sessions, memories, conversation databases, browser state, scheduler output,
usage ledgers and caches remain mutable. They are not recreated by Home Manager.
The managed marker disables Hermes's imperative configuration/update paths.
Intentional configuration changes must be recorded in the private declaration.

## Update the declaration

Review the intended settings before capturing them. Export encrypts directly
through SOPS without writing a plaintext intermediate file:

```sh
cd ~/.dotfiles
direnv exec . python3 home/config/local-llm/hermes_export.py \
  "$HOME/.hermes" "$HOME/Repositories/nix-config/secrets/relay-hermes.json"
```

The private repository's SOPS recipient policy applies. Review changes privately,
verify decryption and the declaration tests, then commit and push the encrypted
input. Update only the `nix-config` lock in dotfiles and build the paired Relay
system and Home Manager generation. Use `hmswitch` for approved activation; the
current wrapper deploys the matching Darwin system first. The gateway restart
can interrupt Telegram work, so wait for running tasks to finish.

Hermes upgrades are a separate reviewed flake-input update, not `hermes update`.
Keep rollback generations and private state backups until the new runtime has
passed CLI, gateway and Qwen checks. Do not run a second gateway against the live
home for testing: use an isolated temporary home and disable scheduled work.

## Checks

```sh
direnv exec . python3 -m unittest discover -s tests -p 'test_hermes_declaration.py'
direnv exec . python3 -m unittest discover -s tests -p 'test_local_hermes_profile.py'
direnv exec . nix flake check
direnv exec . nix build --no-link \
  .#homeConfigurations.william-darwin.activationPackage \
  .#darwinConfigurations.relay.system
```

Configuration evaluation and fixture tests are not evidence of a running
Telegram connection or logout/reboot persistence. Verify those separately after
activation, without sending an unsolicited message or running financial jobs.
