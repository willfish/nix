# Pi MCP servers

Plain `pi` on every Home Manager host and `qwen-pi` on Andromeda and Relay use
the shared MCP server configuration:

| Server | Existing wrapper |
| --- | --- |
| Jira | `mcp-jira` |
| GitHub | `mcp-github` |
| Browser | `mcp-agent-browser` |
| Terraform | `mcp-terraform` |
| NixOS | `mcp-nixos` |
| Filesystem | `mcp-filesystem` |
| DAP debugger | `mcp-dap` |
| Slack | `mcp-slack` |
| AWS access portal | `mcp-aws-access-portal` |

Run `hmswitch` after pulling the dotfiles on another machine. Restart Pi, or
use `/reload` in an existing plain Pi session. The browser server requires
the configured Brave browser to be running with CDP on port 9222. Playwright
MCP is not installed. Authenticated
servers use local sops secrets or session credentials through their wrappers.
No credentials are copied into Pi's configuration.

`mcp-filesystem` is the official `@modelcontextprotocol/server-filesystem`
package from nixpkgs. It is registered on every host. The wrapper allows the
home directory and `/tmp`, both read-write, including secrets under home. Its
search matches file names, not file contents. Use ripgrep for content search.
Paths outside those roots, including `/nix/store` and `/srv`, are refused.

`mcp-aws-access-portal` signs into the TransformUK IAM Identity Center portal
through the visible Brave debugger on port 9222, then writes short-lived role
credentials to a mode 0600 env file. It does not return secret values. It opens
a background tab only when login is required, never focuses that tab, and closes
the tab it opened. Exporting a production administrator role blocks on a local
fuzzel approval prompt. Callers must not dismiss that prompt. Restart Pi after
`hmswitch` so the new server is registered.

`mcp-dap` is a compiled Go server from the Delve project. It is registered only
on development homes. It can spawn debuggees and evaluate expressions in them,
so treat it like a shell. It is not a Ruby or Python IDE debugger; those still
need a language DAP adapter on `PATH`. The wrapper puts Delve on `PATH` for Go.
Build or substitute the binary with `nix build .#mcp-dap-server`. Do not add npm
MCP debugger packages to this repository.

## Using the servers

Ask Pi naturally, for example: "Use the GitHub MCP to read the README in
willfish/nix." The model has one `mcp` tool that can connect, search, inspect
schemas and call the existing servers' tools.

On a fresh profile, the adapter connects once to populate its metadata cache.
Later sessions connect lazily. A disconnected server in `/mcp` is normal before
first use. If discovery failed, or you have added a server, connect before
searching to refresh its metadata. You can also do this yourself:

```text
/mcp
/mcp reconnect github
/mcp tools
/mcp reconnect
```

The last command connects all configured servers. The model's equivalent is
`mcp({connect: "github"})`, followed by `mcp({search: "read file"})`,
`mcp({describe: "github_get_file_contents"})` and a call with the described
arguments. Connections close after ten idle minutes and when Pi exits.

## Managed configuration

`home/user/llm-mcps.nix` is the canonical server list for all three agents.
`home/user/pi.nix` generates `~/.config/mcp/mcp.json` and installs the extension
at `~/.pi/agent/extensions/mcp`. The `qwen-pi` launcher explicitly loads it
because that profile disables automatic extension discovery. Both profiles
read the shared server configuration; each keeps its own writable metadata
cache in its agent directory.

Make permanent changes in the Nix files. The shared JSON is a Home Manager
symlink. The adapter also supports project `.mcp.json` and `.pi/mcp.json`
overrides. `/mcp enable` and `/mcp disable` write project overrides.

The [upstream adapter](https://github.com/nicobailon/pi-mcp-adapter) is pinned
to commit `8243eba3421e301c88c047444f34ab7d5d57163e`, which includes Pi 0.85
compatibility. Nix builds its public helpers with lifecycle scripts disabled.
The dependency lock patch supplies six missing registry integrity hashes;
dependency versions and URLs are unchanged. Server runtimes retain their
existing packaging and runtime credential lookup.

Individual tools and the adapter's extra scripting tool are disabled in favour
of the single discovery proxy. All configured server tools remain callable
through it. Requests have a 120-second timeout. Classic MCP negotiation keeps
the existing servers compatible without extra discovery subprocesses.

## Verification

Build the appropriate Home Manager activation package, then run the adapter
test against the built extension and installed Pi:

```sh
direnv exec . nix build \
  '.#homeConfigurations."william@andromeda".activationPackage' \
  --out-link /tmp/pi-mcp-home
PI_MCP_TEST_EXTENSION="$(readlink -f \
  /tmp/pi-mcp-home/home-files/.pi/agent/extensions/mcp)/index.ts" \
  direnv exec . python3 tests/pi-mcp-runtime.py
direnv exec . node --test tests/local-pi.test.ts tests/pi-openai.test.ts
```

The adapter regression uses isolated profiles, a local model fixture and a
local MCP fixture. It does not use production credentials or call external
services. Live verification should use server initialization, tool discovery
and read-only calls.
