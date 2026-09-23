# Headless macOS nodes

Use nix-darwin for system services and Home Manager for the user's CLI and agent
configuration. Required services must not depend on a graphical login. Apple's
login window and WindowServer remain intact. Safari remains built in; Nix does
not install it. Ghostty, Brave and their desktop configuration are
excluded from the headless profile. SSH uses the connecting machine's terminal.

## Ownership

| Layer | Configuration |
|---|---|
| Node identity | `darwinHosts` in `flake.nix`, `system/darwin/relay.nix` |
| Server composition | `system/darwin/headless.nix` |
| Application jobs and dependencies | `services.nix` |
| Native SSH and login policy | `access.nix` |
| Power and updates | `power.nix`, `maintenance.nix` |
| Local health and log retention | `monitoring.nix` |
| Preflight, configuration diff and deployment | `activation.nix` |
| Encrypted data and native SOPS integration | Private `nix-config` input |
| CLI tools, Pi, accounting/email and agent configuration | Home Manager |

Each registered node gets `darwinConfigurations.<node>` and
`homeConfigurations."william@<node>"`. `william-darwin` remains Relay's alias.
Determinate owns Nix and its configuration (`nix.enable = false`). Keep existing
SSH authentication, accounts, timezone, documentation and rollback tools. There
is no wholesale SrvOS import, broader Nix trust, automatic account migration or
new per-host decryption identity.

## Startup contract

1. System jobs wait for the Nix volume before invoking store executables.
2. Native `sops-nix.darwinModules.sops` installs selected secrets as root, with
   files owned by the existing service user. Explicit key paths retain the shared
   SSH-derived age identity; neither recipients nor ciphertext change.
3. A thin runtime adapter serializes native boot/activation installs and publishes
   `/var/db/dotfiles/secrets-ready.json` only after success. This file contains
   boot identity and manifest path, not secrets. Failed installs invalidate it;
   the boot job retries. Build validation uses the unwrapped upstream installer.
4. Home Manager waits for native readiness, retargets its compatibility symlink
   to `/run/secrets`, and marks its generation ready only after all activation
   steps finish. Old plaintext generations are not erased.
5. Hermes, inference and assistant tools run unprivileged with explicit
   environments. They await both current native secrets and the completed home
   generation pinned by the system configuration.

**Deploy system and home as a pair whenever the home generation changes.** The
home guard rejects a mismatched installed system. Otherwise a standalone home
update could leave boot jobs expecting an older generation. After the initial
handover, type `hmswitch`: it builds the host's Darwin system as your user,
deploys it through `darwin-deploy` if the active system, profile or pinned home
is different, then activates that system's exact home generation. Sudo may prompt
in your terminal. A failed system deployment stops the wrapper before home
activation; preflight and the matching-generation guard remain mandatory.

`hmswitch --dry` builds the pair and previews home changes without activation or
sudo. `--ask` requests confirmation before a needed system deployment and again
before home activation. The paired path also accepts `--verbose`, `--quiet`,
`--no-nom` and `--diff` (and their short forms). Other arguments are rejected
before building; update flake inputs separately so both stages share one target.
Legacy Darwin and Linux argument forwarding is unchanged.

A system-only change whose home generation is unchanged does not require a new
home activation. Readiness gates startup, not live credential rotation: restart
credential-caching consumers explicitly when necessary.

Accounting wrappers use pinned Playwright Chromium headless shell with explicit
`CHROME_PATH` and `PLAYWRIGHT_BROWSERS_PATH`. No startup browser downloads occur.
Telegram bot/MCP integrations remain; no desktop client is required. Human
account-authentication gates remain in force, with no automatic GUI fallback.

## Local operations policy

`darwin-health` reports service state, readiness, memory-page counters and swap.
It does not print raw launchd environments or read secret contents. It does not
contact an external telemetry service or open a listener. Socket-activated SSH
is checked for a loaded job, not continuous process activity; this is not an
end-to-end SSH, browser or model request test.

A low-priority system job runs every five minutes. It records protected status
in `/var/db/dotfiles/health.json` and maintains only allowlisted logs in
`/var/log/dotfiles`. Logs exceeding 5 MiB are copy-truncated, retaining two tail
archives of at most 5 MiB each. Logs can grow between checks; concurrent writes
during truncation can be lost. This avoids signalling services that cannot reopen
stdout. It is operational retention, not a lossless audit trail. No old user logs,
credentials, browser sessions, backups or unrelated caches are deleted.

Update checks are enabled daily; automatic downloads, app updates and macOS
upgrades are disabled by this profile. Existing Apple security/config-data policy
is left unchanged. Managed-device policies may take precedence. There are no
scheduled reboots, automatic erases, Nix GC or new optimisation jobs. Keep OS
upgrades, restarts and any broader maintenance policy explicitly approved.

The managed Hermes weekly updater is check-only when `HERMES_SYSTEM_SERVICE=1`.
It cannot apply an update that might recreate the old user-managed gateway.
Apply Hermes runtime updates and restart its system service during approved
maintenance; the legacy workstation updater retains its existing behaviour.

## Build without activation

Use native macOS SSH: `ssh william@relay.local`, not Tailscale SSH. Verify an
unfamiliar host key through a trusted connection or local console.

```bash
cd ~/.dotfiles
direnv exec . nix build --no-link .#darwinConfigurations.relay.system
direnv exec . nix build --no-link '.#homeConfigurations."william@relay".activationPackage'
direnv exec . nix flake check
```

Use a Git-tracked staging checkout for remote builds, excluding `.direnv` and
other local state from the flake source. Builds activate nothing. Linux can check
configuration boundaries and offline logic; native builds and synthetic browser
tests still do not establish pre-login operation.

## First handover: separate maintenance approval required

Arrange native LAN SSH, a second session and local recovery access. Keep the
current graphical session during this handover. Do not combine it with logout,
reboot, credential cleanup or Nix replacement. Sudo remains interactive, without
blanket passwordless authorization.

1. Verify clean checkouts, pinned inputs and both built targets. Save current
   home/system references, job definitions and enabled states in a protected
   directory outside Git. Directory mode 0700 and file mode 0600 are appropriate.
2. Disable/unload and archive the old user jobs so a later GUI login cannot start
   duplicates: `ai.hermes.gateway`, `org.nix-community.home.sops-nix`,
   `org.nix-community.home.local-llm`, and
   `org.nix-community.home.local-assistant-tools`.
3. Unload/archive the unmanaged `io.tailscale.tailscaled` and `limit.maxfiles`
   system definitions. Retire `org.nixos.dotfiles-secrets` too if an earlier
   custom-provisioner version was deployed. Preserve Tailscale state. Its restart
   can interrupt overlay access, so remain on native LAN SSH. Inventory old
   OpenClaw, Homebrew and Docker definitions separately before removing anything.
4. Build as the SSH user and capture the exact system output. Run its read-only
   preflight, then, only with deployment approval, its deploy command:

   ```bash
   system=$(direnv exec . nix build --no-link --print-out-paths .#darwinConfigurations.relay.system)
   sudo "$system/sw/bin/darwin-preflight" --target "$system"
   sudo "$system/sw/bin/darwin-deploy" "$system"
   ```

   Preflight checks identity, account, private-key permissions, required runtime
   files, legacy jobs and secret-path ownership, and shows a metadata-only policy
   diff. It does not stop services. The deploy command repeats preflight under a
   lock before changing the system profile. It uses the installed Nix CLI and
   the built target's `darwin-rebuild activate`, without fetching private inputs
   as root. Never copy an SSH private key into root's home as a shortcut.
5. Run `direnv exec . bash home/config/bin/hmswitch` from the updated checkout.
   Application jobs may be waiting for this matching home to finish. The wrapper
   neither recreates the unmanaged system plists nor changes Nix cache settings.
6. Inspect `darwin-health` and perform functional checks before considering logout.

An unmanaged directory at the old secret-symlink path is a blocker, not permission
to delete it. Never bypass preflight or the matching-generation guard merely to
make a switch succeed.

### Recovery

`darwin-deploy` saves system/home generation references under
`/var/db/dotfiles/rollouts` before profile registration. On failure, it attempts to
restore the previous system profile pointer, provided another deployment has not
changed it. For a first install it removes only its newly registered pointer.

**This is not a transactional rollback of macOS or running services.** Activation
may already have changed jobs or `/etc`. Inspect the saved references and restore
the previous system, home and archived job definitions deliberately. Stop new
jobs before restoring old ones; never run two Hermes gateways or Tailscale
instances. Restore previous enabled states, retain native SSH, and use local
recovery when necessary. First-time installation has no previous Darwin system;
retain the nix-darwin uninstall/recovery route and the original home/plists.

## Acceptance after deployment

Each disruption requires separate approval. Verify services with the graphical
session still present, then after an approved logout, and again after an approved
reboot before any graphical login:

- Native SSH and sudo administration work; active system/home match the targets.
- Private `check.py --runtime --groups coding accounting email telegram` validates
  selected values/permissions without printing them. Native SOPS succeeds and the
  three application jobs run as the intended unprivileged user.
- Pi and agent-browser remain available; MCPs are GitHub, NixOS and Telegram.
- An offline synthetic browser page renders. A real local inference request
  succeeds with GPU offload, not just a healthy listener.
- Restart/network recovery works. Health snapshots advance without exposing
  credentials. Compare memory pressure and swap at equivalent workload/model
  state, not by summing RSS or treating reclaimable cache as wasted memory.

## New nodes and upstream references

The registry currently targets Apple Silicon. Add a node module importing
`headless.nix`, register it in `darwinHosts`, and extend capability regressions.
Keep hardware/model differences explicit. Bootstrap macOS, the existing account,
native Remote Login, compatible Nix and secure repository/key access separately.
Decide FileVault policy explicitly: this configuration cannot bypass preboot
unlocking after power loss.

Relay's Hermes runtime and configuration are managed as described in
[Hermes reproducibility](hermes-reproducibility.md). Its encrypted declaration
is Relay-specific; do not copy it to another node or enable its scheduled jobs
there. Provision required model files separately. Conversation and execution
history need state backups, not a configuration rebuild. This is not a bare-metal
macOS installer. Privacy grants and OS management remain separate.

The patterns are informed by [SrvOS](https://github.com/nix-community/srvos/tree/ee2f679bdc7324f90dc73c0f22f2d5fab25b1b33/darwin),
[Nix Community's Mac builders](https://github.com/nix-community/infra/tree/6d9ddcdfc42fc0d449fc620de9ed1115b2ba994d/modules/darwin),
and native sops-nix already pinned by this flake. Their automatic reboots,
restrictive SSH-key lookup, broader Nix trust and destructive CI cleanup are not
copied. Determinate's ephemeral provisioning is a useful future MDM reference,
not authorization to adopt its autologin or erase workflow.
