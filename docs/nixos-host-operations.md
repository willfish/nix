# NixOS Host Operations

This is the short operational reference for the four NixOS hosts and the shared Home Manager configuration. Run commands from the dotfiles repository unless noted otherwise.

## Hosts and roles

| Host | Role | Notable host-specific configuration |
|---|---|---|
| `andromeda` | workstation | System76 hardware, RTX 5090 with NVIDIA 595.99.02 open driver, Steam, Linux 6.18 |
| `starfish` | workstation | Dell Precision laptop |
| `foundation` | workstation | Framework AI 300 hardware and patched MT7925 driver |
| `terminus` | headless server | ZFS media pool, Immich, Audiobookshelf |

All hosts import `system/modules/base.nix`. Workstations additionally import `system/modules/workstation.nix`; Terminus imports `system/modules/server.nix`.

## Evaluate and build

Evaluate a host without realising its closure:

```bash
nix eval --raw .#nixosConfigurations.terminus.config.system.build.toplevel.drvPath
```

Realise one host or all hosts:

```bash
nix build -L .#nixosConfigurations.terminus.config.system.build.toplevel --no-link

for host in andromeda starfish foundation terminus; do
  nix build -L ".#nixosConfigurations.${host}.config.system.build.toplevel" --no-link
done
```

Run repository-wide checks:

```bash
nix fmt -- --ci
nix flake check -L
python3 home/config/llm/scripts/audit-skills
nix build --no-link .#homeConfigurations.william-linux.activationPackage
direnv exec . python3 scripts/check-behavior.py "$(nix eval --raw .#homeConfigurations.william-linux.activationPackage.outPath)"
```

The behavioral gate discovers all Python, Node and Bats suites and runs MCP
and Pi runtime regressions against the selected immutable generation. Unexpected
skips fail the gate. Live Herdr/voice tests and the deployed, host-specific Qwen
launcher check remain explicit opt-ins; the offline gate never touches live panes.
The development shell supplies Jinja2, mitmproxy and private D-Bus test dependencies.

## Home capabilities

`home/user/capabilities.nix` assigns a purpose to each host and exposes typed
`dotfiles.capabilities` switches. Consumers use those switches rather than
repeating hostname tests. Hardware-specific configuration remains separate.

| Home | Purpose | Capabilities |
|---|---|---|
| Foundation, Andromeda | Full workstations | Work/cloud, knowledge base, personal/accounting, development and desktop |
| Relay (`william@relay`, alias `william-darwin`) | Headless automation server | Pi, GitHub, Hermes/Telegram, accounting, email, documents, headless browser automation; built-in Safari remains available |
| Terminus | Self-maintaining NAS | Pi, Git/SSH, Nix maintenance, diagnostics, NAS import tools and agent-browser CLI |
| Starfish, generic Linux | Legacy, pending classification | Preserve existing workstation capabilities |

Pi and its existing model credentials remain on every home. Agent-browser CLI
also remains everywhere; its visible-Brave MCP wrapper is not registered on
Relay or the NAS. No browser is installed or launched implicitly on the NAS.
Relay's automation uses a pinned headless browser instead of Brave. Full profiles
retain visible-browser integration; Starfish preserves its previous capabilities.
See [Headless macOS nodes](headless-darwin.md) for the system-service handover,
new-node provisioning and the separate logout/reboot verification gates.

The MCP catalogue drives registration and wrapper/package installation together.
Work skills and knowledge-base timers disappear with their capability. Specialist
editor tools, AWS tools, compilers, document tools and personal apps are not part
of the common baseline. Repository-specific development dependencies belong in
project dev shells, including on Relay when automation runs project checks.

The private module renders only the selected secret groups from the existing
shared encrypted file. This is operational separation, not cryptographic
isolation: every machine retains the same effective decryption key. Existing
browser sessions, copied credentials, work caches, imperative Hermes scripts and
old generations are not automatically deleted. Audit and approve cleanup of that
state separately; an absent capability is not evidence that all past data is gone.

Run `nix build .#checks.x86_64-linux.host-capabilities` to evaluate inclusion and
exclusion assertions for every home, including Darwin. Use native builds and
runtime checks before activation; evaluation alone does not validate services.
Terminus no longer enables dconf or desktop Stylix activation during SSH switches.
Remote activation and any cleanup of stored data require explicit authorization.

## Private configuration input

`nix-config` pins `willfish/nix-config` over SSH. It supplies encrypted SOPS files,
recipient policy, work integrations, and private Home Manager/NixOS modules.
Building this personal configuration requires authenticated access to that input;
the public source is not a standalone configuration for other users. Keep the
input and its test logs private. CI access to it is not configured here.

Moving files does not remove historical copies. Before publishing an existing
repository, sanitize every branch and tag and resolve retained pull-request refs,
CI artifacts and cached views with the hosting provider. Do not assume a clean
checkout or a force-push makes the repository safe to publish.

Clone it to `~/Repositories/nix-config` or set `NIX_CONFIG_ROOT` for editing
helpers. Edit with SOPS in that checkout, add new Home Manager secret names to
`secret-groups.json`, then commit and push. In dotfiles, run
`direnv exec . nix flake update nix-config`, build, and `hmswitch`. SSH authorized-key
changes also require a NixOS rebuild. Commit the verified lock-file update.
Other hosts need SSH repository access; the input contains no decryption keys.
Never put plaintext secrets in either flake: inputs can enter the Nix store.

## Slack session credentials

Treat `~/.config/slack-session/tokens.env` as private data, never as a shell
script. Consumers parse only the expected literal token assignments; legacy
quoted or `export` assignments remain readable without executing other lines.
A successful `slack-refresh-session` authentication atomically replaces the local
file with canonical assignments. A failed authentication keeps the previous file.
OAuth requests and encrypted-secret updates pass credentials through stdin rather
than command-line arguments.

Ignored editor files under the private checkout's `secrets/.conform*` are still
plaintext if present.
Ignoring them prevents accidental staging, not disclosure. Investigate any exposure
and confirm credential rotation separately from a configuration switch.

## Herdr plugin revisions

`home/config/herdr/plugins.json` pins each source repository and commit. After
reviewing an upstream revision, update its pin and switch Home Manager. Activation
checks installed provenance and reconciles only mismatches using `--ref`; it
checks the resulting state rather than trusting the install command alone.

An installation failure warns instead of blocking an otherwise usable offline
switch. Inspect `herdr plugin list --json` before retrying. Herdr stages builds
before replacement and attempts rollback on registration failures, but rollback
can itself fail. The activation warning is not a guarantee that every plugin
remains unchanged.

## Test, switch, and roll back NixOS

On the target NixOS host, test a generation without making it the boot default:

```bash
nh os test .
```

After the smoke checks pass, activate it persistently:

```bash
nh os switch .
```

List and select earlier generations:

```bash
sudo nix-env --list-generations --profile /nix/var/nix/profiles/system
generation=123
sudo "/nix/var/nix/profiles/system-${generation}-link/bin/switch-to-configuration" switch
```

If a generation cannot boot, select an earlier generation from the systemd-boot menu. Treat rollback as incident recovery: verify SSH/Tailscale and the host's critical services before making further changes.

### Andromeda RTX 5090 installation

The driver is pinned to 595.99.02 for NVIDIA's DIFR suspend/resume fix. The
previous systemd suspend/resume service configuration is retained. Repeated
suspend/resume testing with the installed card is still required to confirm
that the earlier freeze is resolved.

Build and stage the configuration for the next boot before shutting down to
install the card. Use `boot` so the running graphical session is not restarted:

```bash
direnv exec . nix build -L .#nixosConfigurations.andromeda.config.system.build.toplevel --no-link
sudo nixos-rebuild boot --flake .#andromeda
```

After installing the card and booting, confirm the loaded driver:

```bash
nvidia-smi --query-gpu=name,driver_version --format=csv
cat /proc/driver/nvidia/version
systemctl --failed
```

The shared voice services use CUDA for TTS and NVIDIA Vulkan for Whisper on
Andromeda, and Radeon Vulkan on Foundation, configured in `home/user/voice.nix`.
Run `hmswitch` after the GPU swap to activate the matching speech configuration.
See [voice controls](voice.md) for setup and recovery. On supported hosts the
controller starts at login, while speech backends warm on session attachment.
Interactive Pi sessions in Herdr attach automatically; after installing, use
`/reload` once in existing standard Pi sessions or restart Qwen sessions.
Selection stays sticky: use the tray or Super+Shift+V to choose a destination.

## Home Manager

Use `hmswitch` for normal activation; it chooses `william-darwin`, a known `william@<host>` Linux configuration, or `william-linux`:

```bash
hmswitch
```

For manual builds, choose the platform explicitly:

```bash
# Linux
nix build .#homeConfigurations.william-linux.activationPackage --no-link

# macOS, run on relay or in the macOS CI job
nix build .#homeConfigurations.william-darwin.activationPackage --no-link

# Host-specific Linux configuration
nix build '.#homeConfigurations."william@terminus".activationPackage' --no-link
```

If invoking `nh` directly, pass the flake path and configuration separately:

```bash
nh home switch . --configuration william-linux
```

Do not use `nh home switch '.#william-darwin'`; `nh` interprets that as a package-style attribute rather than a Home Manager configuration selection.

### Relay: use native macOS SSH for switching

Use `ssh william@relay.local` on the LAN for Relay Home Manager activation,
not the Tailscale SSH route through `ssh relay`. macOS Remote Login and Tailscale
SSH have different privacy-permission contexts. A successful build or earlier
switch does not prove that application updates will work over Tailscale SSH.

```bash
ssh william@relay.local 'bash -s' <<'SH'
set -euo pipefail
export PATH="/nix/var/nix/profiles/default/bin:$HOME/.nix-profile/bin:/etc/profiles/per-user/$USER/bin:$PATH"
cd "$HOME/.dotfiles"
direnv exec . nix build --no-link .#homeConfigurations.william-darwin.activationPackage
# First headless deployment requires the nix-darwin handover below.
# Deploy the matching system before any new home generation, then use:
direnv exec . bash home/config/bin/hmswitch
SH
```

For Relay's first headless deployment, follow the
[headless macOS handover](headless-darwin.md#first-handover-explicit-maintenance-approval-required).
Build and deploy the matching system before each new home generation. The home
guard checks that pairing; `hmswitch` never installs or restarts system services.

If macOS refuses to update applications over SSH, stop. Either run `hmswitch`
in a local graphical terminal, or have the user enable **System Settings →
General → Sharing → Remote Login → Allow full disk access for remote users**,
then retry through native SSH. Do not enable that permission automatically.

The LAN hostname may have a separate known-hosts entry. Verify its host key
against Relay's host public key obtained through an already trusted connection
or its local console before accepting it. `ssh-keyscan` alone does not establish
trust; do not disable host-key checking.

After switching, compare the active Home Manager generation with the evaluated
`william-darwin` activation output and verify the selected runtime secrets.
Do not infer successful activation merely from the updated Git checkout.

## Terminus Immich upgrades

Immich server and machine learning use the existing locked unstable package set;
the rest of Terminus, including PostgreSQL, stays on the release channel. This
avoids the insecure release-channel Immich 2 package without an insecure-package
exemption. Review [Immich's v3 migration guidance](https://immich.app/blog/v3-migration)
when upgrading clients and API integrations. Version 3 removes pgvecto.rs;
the NixOS module already configures VectorChord.

Before the first activation of a new major Immich version, save and verify a
PostgreSQL logical backup, preserve the corresponding media, and record the old
NixOS generation. Test restoring that backup separately before proceeding. A
NixOS generation rollback alone does not undo database migrations. Stop if the
live database extensions or restored backup have not been verified. This upgrade
checkpoint does not change the general replaceable-media retention policy.

After activation, verify the Immich service, login, existing photo retrieval,
and a disposable upload. Match mobile clients and any import tools to the new
API version before resuming bulk imports.

## Terminus checks

For routine checks from any managed machine, use the repository Justfile. With no argument it reports every reachable known host; an argument limits it to one host. LAN routes are preferred and Tailscale is the fallback:

```bash
just health
just health terminus
just terminus-health  # detailed ZFS/SMART check; prompts for remote sudo
```

The routine report includes failed systemd units, `zpool status -x`, the next
scheduled ZFS scrub, and `smartd` state when those services exist on a host.

Tailscale SSH may require interactive re-authentication. Once authenticated, collect the read-only pre-activation state:

```bash
ssh -t terminus 'sudo zpool status -P; systemctl --failed; systemctl is-active immich-server audiobookshelf'
```

After `nh os test` or `nh os switch`, repeat that command and additionally confirm the services over LAN or Tailscale. Include the ZFS and SMART health checks:

```bash
ssh terminus duf
ssh -t terminus 'sudo zpool status tank'
ssh -t terminus '
  for disk in /dev/sda /dev/sdb /dev/nvme0; do
    echo "== $disk =="
    sudo smartctl -H "$disk"
  done
'
```

Healthy ZFS output reports `state: ONLINE`, a successful last scrub, and `errors: No known data errors`. Healthy SMART summaries report `PASSED` or `OK`. Inspect recent service history when diagnosing a warning:

```bash
ssh terminus 'journalctl -u smartd --since "30 days ago" --no-pager'
ssh terminus 'journalctl -u zfs-scrub.service --since "90 days ago" --no-pager'
```

## CI

The `CI` workflow runs formatting, flake checks (including Home Manager headless-profile assertions), the skill audit, and the complete offline Python/Node/Bats and packaged-runtime gate. It also evaluates every Home Manager configuration, builds every Linux and Darwin Home Manager home, and realises one build per NixOS host. Find current runs with:

```bash
gh run list --workflow ci.yml --limit 10
run_id=123456789
gh run view "$run_id" --log-failed
```

Commands involving `sudo`, activation, rollback, or Terminus services must run on the relevant host. The macOS activation package must be realised on `relay` or by macOS CI.
