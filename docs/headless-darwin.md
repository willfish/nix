# Headless macOS nodes

Use nix-darwin for system services and Home Manager for the user's CLI and agent
configuration. Required services must run without a graphical login. Apple’s
login window and WindowServer are not disabled. Ghostty and its configuration
remain available for occasional local maintenance, without automatic launch.

## Ownership

- `system/darwin/headless.nix`: native SSH, power policy, system launchd jobs,
  file limits and Tailscale. `system/darwin/relay.nix` supplies node identity.
- `flake.nix`: register nodes in `darwinHosts`. Each gets a Darwin system and a
  `william@<node>` home. `william-darwin` remains Relay's compatibility alias.
- Home Manager: Pi, shell, email/accounting tools, documents and Ghostty. The
  headless profile excludes Brave, AeroSpace and visible-browser MCP wrappers.
- Private config: selected secrets and their shared SSH-derived age identity.
  Nothing changes the existing decryption authority or rotates credentials.

Hermes, local inference and assistant tools are system LaunchDaemons running as
`william`, not root. Each waits for the private provisioner's readiness marker,
which must match both the current boot and secret installer. A failed provision
invalidates readiness. Startup retries are bounded per attempt and launchd
retries failed jobs. This gates startup, not live credential rotation: restart
consumers explicitly after changes when they retain credentials in memory.

The root-only jobs are Tailscale and the global file-limit adjustment. Preserve
Tailscale's existing state and socket paths during handover. Do not run competing
Homebrew and nix-darwin instances against the same state.

Determinate retains ownership of Nix and its configuration (`nix.enable = false`).
The headless `hmswitch` path does not rewrite Nix caches or install system plists.
System changes require a separate `darwin-rebuild`. Existing cache settings are
preserved, not newly managed by nix-darwin.

Accounting wrappers use the hash-pinned Playwright Chromium headless shell from
the existing nixpkgs input. No browser download is performed at service startup.
`CHROME_PATH` and `PLAYWRIGHT_BROWSERS_PATH` are explicit in the gateway and managed
wrappers. Telegram bot/MCP integrations remain; a desktop client is not required.
Interactive authentication challenges still require an approved human workflow,
not an automatic fallback to a visible browser or bypass of an authorization gate.

## Build without activation

On Relay, use native macOS SSH via `ssh william@relay.local`, not Tailscale SSH.
Verify an unfamiliar LAN host key through an already trusted connection or local
console. Do not disable host-key verification.

```bash
cd ~/.dotfiles
direnv exec . nix build --no-link .#darwinConfigurations.relay.system
direnv exec . nix build --no-link '.#homeConfigurations."william@relay".activationPackage'
direnv exec . nix build --no-link .#checks.aarch64-darwin.headless-darwin
```

A separate staging checkout allows native builds without changing the active
checkout. Builds do not activate either configuration. Linux can run the
`headless-darwin` and `host-capabilities` evaluation checks, but cannot establish
macOS runtime or pre-login compatibility.

## First handover: explicit maintenance approval required

Arrange native LAN SSH, a second session and local recovery access. Keep the
current graphical session until the system service handover is verified. Do not
combine this step with a logout, reboot, credential cleanup or Nix replacement.
Administrative commands require interactive sudo authorization; do not introduce
blanket passwordless sudo.

1. Recheck clean checkouts, pinned inputs and built target paths. Record the
   current home/system generation, relevant service state and current memory
   pressure/swap. Save old service definitions and rollback references in a
   protected directory outside Git (directory mode 0700, files 0600).
2. Disable and unload the old user jobs, then archive their plists so a later
   graphical login cannot start duplicates:
   `ai.hermes.gateway`, `org.nix-community.home.sops-nix`,
   `org.nix-community.home.local-llm`, and
   `org.nix-community.home.local-assistant-tools`. Inspect both loaded state and
   installed definitions. Preserve Hermes data, browser sessions and all keys.
3. Unload and archive the unmanaged system plists `io.tailscale.tailscaled` and
   `limit.maxfiles`. Stopping Tailscale can disconnect overlay sessions: do this
   through native LAN SSH. Do not delete its state. Inventory old OpenClaw,
   Homebrew and Docker definitions separately before approving their removal.
4. Activate the exact system built as the SSH user, without making root fetch
   private inputs. The pinned nix-darwin supports `activate` from its built
   system path. After verifying the target path, register that generation with
   `sudo "$(command -v nix-env)" -p /nix/var/nix/profiles/system --set "$system"`,
   then run `sudo "$system/sw/bin/darwin-rebuild" activate`, where `system` is
   the output of `direnv exec . nix build --no-link --print-out-paths
   .#darwinConfigurations.relay.system`. Profile registration precedes activation;
   restore the previous profile too if activation fails. Do not fetch an unpinned
   installer or copy a private SSH key into root's home.
5. Run the checkout's updated wrapper, not the old deployed wrapper:
   `direnv exec . bash home/config/bin/hmswitch`. It chooses `william@relay` and
   does not manage system services. Its activation guard requires the matching
   system secret job to be installed first. Do not bypass the guard.
6. Check all required services, the selected secrets, MCP configuration and
   actual local model/browser operation before considering logout.

System activation refuses to take over while the four old graphical jobs remain
installed or loaded, or while the unmanaged system plist files remain. Home
activation refuses to proceed without the matching system secret job. These
checks are intentional; a routine switch must not silently interrupt automation.

### Rollback

Before deployment, retain the old home generation and job definitions, including
their disabled-state inventory. For a failed handover, stop the new jobs before
restoring old definitions. Restore the previous Darwin generation if one exists;
for the first installation, use the documented nix-darwin uninstall/recovery path
rather than assuming an older Darwin generation exists. Restore the old home
and plists, then restore each job's previous enabled state. Do not start two
Hermes gateways or two Tailscale daemons. Keep native SSH reachable throughout.

The compatibility `hmswitch` path in an old generation can recreate the unmanaged
system jobs. Do not run it alongside the new system services.

## Runtime acceptance, each disruption separately approved

- Native SSH and sudo-based administration work without a desktop login.
- Home Manager's active generation matches the intended target.
- `check.py --runtime --groups coding accounting email telegram` passes from the
  private checkout without printing secret values; only selected secrets render.
- `org.nixos.dotfiles-secrets` exits successfully. Hermes, local inference and
  assistant tools run under the intended unprivileged account in system launchd.
- Pi and agent-browser remain available; MCP servers are GitHub, NixOS and
  Telegram. No visible-Brave registration or AeroSpace configuration remains.
- A synthetic headless page renders without accessing real accounts. Perform a
  local model request and verify GPU offload, not just a healthy HTTP listener.
- After an approved logout, then an independently approved reboot, repeat the
  checks before any graphical login. Test service restart/network recovery too.
- Compare idle memory pressure and swap at equivalent workload and model state.
  Do not sum RSS or count reclaimable filesystem cache as wasted RAM.

A browser launched over SSH while the desktop remains logged in does not prove
pre-login operation. Similarly, successful builds do not prove GPU or keychain
access from a system daemon.

## Adding a node

The registry currently targets Apple Silicon. Create a per-host module importing
`headless.nix`, set its hostname and register it in `darwinHosts`. The shared home constructor assigns the
`automation` role. Keep per-node hardware/model differences explicit rather than
copying Relay's whole configuration. Add that home to capability regressions.

Initial provisioning still requires macOS setup, an existing `william` account,
native Remote Login, a compatible Nix installation and secure access to both
private repositories. Provision the approved shared decryption identity outside
Git. Decide FileVault policy explicitly: an encrypted disk can require local
preboot unlocking after power loss; this configuration does not bypass it.

Hermes currently uses the existing `.hermes/hermes-agent/venv` installation and
mutable job/configuration state. A new node must provision that runtime and its
approved jobs before enabling the gateway. Do not copy Relay's credentials,
caches or job state wholesale. This is declarative service management, not a
complete bare-metal macOS or Hermes installer. Privacy grants, secure bootstrap,
OS updates and boot-time acceptance remain separate operational steps.
