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

### Automatic push gate

Entering the dev shell (`direnv allow` or `nix develop`) installs the existing
commit hooks and a separate `pre-push` hook. The push gate checks clean snapshots
of the pushed tips, not dirty files or whichever branch is checked out. Failures
block the push; checks never activate a generation or contact hosts to deploy it.
An existing unmanaged push hook is not overwritten.

Selection is maintained in `scripts/check-affected.py`:

| Changed paths | Configuration checks |
|---|---|
| `system/<linux-host>/` | That NixOS host only |
| `system/modules/workstation.nix` | Andromeda, Foundation and Starfish |
| `system/modules/server.nix` | Terminus |
| Other `system/modules/` files | All Linux systems |
| `system/darwin/` | Relay system, home and headless assertions |
| `home/` | All distinct homes, profile assertions and Relay's home-dependent system |
| Flake, lock file or unmapped paths | All systems, homes and profile assertions |
| `docs/`, `plans/`, `.github/`, root README, AGENTS and gitignore | No configuration checks |

Selected configurations are evaluated, then built on their native platform.
Other-platform targets are explicitly reported as evaluation-only; those still
need native builds before deployment. The existing flake `pre-commit` check runs
for configuration changes. Shared changes can legitimately select every host.
Update the mapping when adding hosts or changing import boundaries.

New branches compare commits against the remote's advertised tips, not stale
tracking refs. Unavailable local history widens selection; an unavailable remote
or missing old tip blocks the check. Multiple pushed refs are checked independently
and identical tips are combined. Deletions are skipped.

For pre-commit verification, use the same gate on the current worktree:

```bash
direnv exec . dotfiles-pre-push --base HEAD --plan  # selection only
direnv exec . dotfiles-pre-push --base HEAD         # evaluate and build
```

The plan includes tracked and untracked changes. Stage intended new files before
running checks: manual builds use the Git flake so ignored files and private local
state never enter the source. Push checks always use committed sources. The hook
automates configuration checks, not the offline behavioral gate or post-activation
runtime and service checks below. Local hooks remain bypassable
and are not a substitute for protected remote checks.

Run repository-wide checks when explicitly needed, rather than on every push:

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

## Pi Switchboard on Terminus

The external `agent-bus` flake supplies `services.pi-agent-bus` and
`programs.pi-agent-bus`. Only Terminus runs the hub. Participating homes install
one client at `~/.pi/agent/extensions/agent-bus`; do not also use `pi install`.
The existing `~/.local/bin/pi` wrapper remains the sole runtime token loader.
Qwen's separate offline profile and explicit extension list exclude Switchboard.
Print, JSON, RPC, offline and `PI_AGENT_BUS_ENABLED=0` sessions do not participate.

The wrapper reads `PI_AGENT_BUS_TOKEN` through `read-sops-secret` before normal
or prompt-capture execution. An explicit token, including an empty value, takes
precedence; a missing secret leaves Pi usable without bus participation. The
URL defaults to `http://terminus:7420`; override `PI_AGENT_BUS_URL` only with a
trusted tailnet destination, never a public HTTP endpoint or credential-bearing
URL. No token belongs in shell startup files, Nix expressions or command arguments.
Prompt capture adds the effective bus hostname to both proxy-bypass variables;
an empty URL uses the client's default. Existing bypass entries are retained.
Capture supports ordinary ASCII DNS names and canonical dotted-decimal IPv4.
It disables bus participation for other URL forms, including Unicode hostnames,
noncanonical IPv4 and IPv6, rather than risking WHATWG normalization sending
presence or mail through the provider proxy. Use an ASCII/punycode tailnet name
or canonical IPv4 address for capture. Normal Pi preserves URL overrides and is
not restricted by this capture-only safeguard.

### Candidate checks before activation

Build the matching home and Terminus system generations using the commands
above, then run the complete behavioral gate against that home. Its wiring
checks read the generated credential wrapper but never execute it in fixtures.
Run the additional composition probe with the immutable Pi, client and history
outputs selected by that same generation, not a different upstream Pi pin:

```bash
python3 tests/pi-agent-bus-composition.py \
  --pi-package "$pi_package" --extension-package "$extension_package" \
  --prompt-history "$prompt_history" --mitmdump "$mitmdump" \
  --ca-bundle "$ca_bundle" \
  --tool-path "$tool_path"
```

Supply absolute Nix store paths. `tool_path` is a colon-separated list of Nix
`bin` directories supplying Bash, flock, fd, ripgrep and coreutils. Bash and
flock are resolved only from that supplied path, never the caller's host PATH.
Use the pinned `cacert` package's certificate bundle for `ca_bundle`; the fixture
must not depend on a host-specific `/etc/ssl` path. It replaces only the CA
bundle path in a temporary copy of the capture script, alongside the pinned
mitmdump substitution; production capture code is unchanged. This does not
validate the deployed capture script's CA path on macOS: verify real Darwin
capture separately before deployment. The probe uses a synthetic environment,
temporary capture logs and loopback endpoints. It checks
real Bun proxy bypass, client/history loading and exclusion modes, not service
reachability or actual credentials. It never executes the generated credential
wrapper. Run it natively on each target platform; alternate loopback-address
availability is a fixture prerequisite, not proof of tailnet connectivity.

Also run the external repository's complete `tests/pi-runtime.test.mjs` against
those selected artifacts and the selected compiled hub. Its required variables
are `PI_AGENT_BUS_TEST_PI_PACKAGE`, `PI_AGENT_BUS_TEST_EXTENSION_PACKAGE`,
`PI_AGENT_BUS_TEST_EXECUTABLE`, `PI_AGENT_BUS_TEST_PYTHON` and
`PI_AGENT_BUS_TEST_TOOL_PATH`. The executable variable names the hub launcher;
the Python variable names the immutable Python executable. Do not substitute a
credential-loading Pi wrapper. This remains separate from native home builds,
actual credential loading and the deployment checks below.

### Health, authentication and network isolation

After authorized activation, run on Terminus:

```bash
systemctl is-active pi-agent-bus
systemctl show pi-agent-bus -p ExecStart -p LoadCredential -p After -p Requires
sudo ss -lntp
```

The service must use the compiled store launcher and systemd `LoadCredential`,
not a source checkout or startup build. Secret ordering depends on SOPS mode:
require `sops-install-secrets.service` only when systemd secret activation is
enabled; activation-script mode installs secrets before service activation.
Expect the HTTP listener on port 7420 and no new EPMD/distribution listener.
Repeat the existing Terminus service/ZFS/SMART checks after activation.

From a managed tailnet client, use a Bash subshell with tracing disabled:

```bash
(
  set +x
  set -euo pipefail
  url="${PI_AGENT_BUS_URL:-http://terminus:7420}"
  curl -q --noproxy '*' --fail --max-time 5 "$url/health"
  curl -q --noproxy '*' --silent --output /dev/null --write-out '%{http_code}\n' \
    --max-time 5 "$url/v1/agents"  # expect 401 without a token
  token="$(read-sops-secret "${SOPS_NIX_SECRETS_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/sops-nix/secrets}/PI_AGENT_BUS_TOKEN")"
  test -n "$token"
  printf 'Authorization: Bearer %s\n' "$token" |
    curl -q --noproxy '*' --fail --silent --show-error --max-time 5 \
      --header @- "$url/v1/agents" | jq '{visibleAgents: (.agents | length)}'
)
```

Health returns only `{"ok":true}`. A successful first agents page proves
credential acceptance, not complete paginated discovery or active SSE receipt.
Use `/agents` and `/bus` in two standard Pi TUIs to verify registration and
receiving. Check labels, model updates without runtime-ID changes, notice inbox
viewing without a model turn, and control off by default. Control requires local
receiver consent; hub acceptance is not delivery or execution acknowledgement.

Resolve `terminus` to its Tailscale address before diagnosing the HTTP service.
Check `tailscale status`, MagicDNS and the selected route; use an explicit
trusted tailnet IP/FQDN if local DNS resolves the short name to the physical LAN.
Test with Mullvad both on and off. Do not open port 7420 globally to fix routing.
The wildcard bind relies on the effective firewall trusting only loopback and
`tailscale0` for this port. From a separate physical-LAN client, probe the
Terminus **LAN address**, including `/health`, with a bounded connection timeout;
the connection must fail. A tailnet success or firewall-source inspection does
not replace this negative check. Stop rollout on any LAN exposure or consent
failure. SSH re-authentication is a separate access gate; stop and obtain user
assistance rather than retrying host authentication automatically.

### Rotation, disablement and rollback

Rotate the shared token in the private encrypted configuration, install it on
the hub and participating homes, restart `pi-agent-bus`, then restart Pi through
the standard wrapper. `/reload` alone does not refresh an inherited token.
Expect existing clients with the old token to stop retrying after 401. Never
print bearer headers or capture real bus traffic while diagnosing rotation.
All token holders share one trust domain and can see presence or impersonate a
peer; cwd, labels and model identifiers are not private between participants.

For temporary client disablement launch `PI_AGENT_BUS_ENABLED=0 pi`. For
persistent removal disable `programs.pi-agent-bus`; disable the Terminus service
with `services.pi-agent-bus.enable = false`. Build and activate the corresponding
generations. Roll back to the reviewed input revision and prior home/system
generations when an upgrade fails, then restart clients and the hub as needed.
A service restart discards queued mail and deduplication state; client reload or
restart also clears inbox state and local control consent. Neither rollback nor
restart makes an uncertain control request safe to resend automatically.

## Verification coverage

This checkout has no CI workflow. The dev-shell hooks provide local configuration
checks; run the offline behavioral gate for behavioral changes and native-platform
builds for any targets reported as evaluation-only before deployment.

Commands involving `sudo`, activation, rollback, or Terminus services must run on the relevant host. The macOS activation package must be realised on a native macOS builder such as `relay`.
