# Herdr from a phone

Ovis runs as William's Home Manager user service on Terminus. Open
<https://terminus.taile09696.ts.net/> with Tailscale connected, then add the page
to the phone's home screen. The phone and Terminus must use the same Tailscale
login. No Ovis account, relay or additional model subscription is required.

```text
Phone → Tailscale Serve (HTTPS) → Terminus Ovis
                                  └─ SSH over Tailscale → Andromeda Ovis bridge
                                                          └─ local Herdr app
```

## Hosts and connections

`dotfiles.capabilities.ovisHub` enables the Terminus hub;
`dotfiles.capabilities.ovisBridge` enables the Andromeda bridge. Both listen only
on `127.0.0.1:7317`. Only Terminus has a Tailscale Serve mapping. Raw Herdr
sockets are never exposed to the network.

In the hub, **Settings → Devices and PCs → Add a connection → SSH** adds a host.
Use `william@andromeda` for this pilot. The hub's account must already have SSH
access and trust the host key. Do not register additional keys or approve a
runtime installation merely to clear a connection error.

The pinned compatibility patch makes `OVIS_SSH_RUNTIME=herdr-core` select the
independently managed bridge at `~/.local/state/ovis/herdr-core.sock`. A missing
bridge fails closed rather than installing the upstream TUI-only remote bundle.
This deployment supports the Herdr app's default core session. The bridge waits
for its existing socket; it does not create a replacement core.

Andromeda must remain awake and reachable for its sessions to work. Stopping
Ovis or losing Terminus does not stop Herdr or its agents. Existing Herdr
plugins, model credentials and conversations remain in place.

## Operations

On either host:

```bash
systemctl --user status ovis
journalctl --user -u ovis -n 50 --no-pager
curl --fail http://127.0.0.1:7317/api/health?scope=bridge
```

On Terminus, inspect the private HTTPS route with `tailscale serve status`.
Provision it once, only when that HTTPS listener is unused:

```bash
tailscale serve --bg --https=443 http://127.0.0.1:7317
```

Never use `tailscale funnel`. A Tailscale owner identity is required before the
hub starts. Unknown remote identities must pair; the service does not enable
the headerless single-owner shortcut. Treat hub access as remote shell access
to its connected machines. Device pairings, SSH state and conversation caches
live under `~/.local/state/ovis`; keep that directory private and out of Git.

Nix owns the package and service. Upstream self-updates, automatic remote-bundle
updates, telemetry and the public relay/direct listeners are disabled. Update
the `ovis-source` input and the release sidecar hash together, rebuild the
package and both home configurations, then activate both hosts with `hmswitch`.
Keep their Ovis versions aligned. A bridge-version mismatch requires a Nix
deployment, not the in-app bridge installer.
