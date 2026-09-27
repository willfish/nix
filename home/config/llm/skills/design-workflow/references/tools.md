# Design tools and bridges

Apply `~/.agents/guides/documentation-relevance.md` to prose.
Use the current repository's pinned environment and source format first. For
Will's machines load `local-dev-environment`; run commands with `direnv exec`
when required. Missing tools use ephemeral Nix, not manifest/host installs.
Authoring and round-trip edits below require implementation scope. Advice/critique
must not change reviewed sources; disposable previews are allowed only when the
requested scope permits them. Report other checks as not run.

## Contents

- [Default capability map](#default-capability-map)
- [Reproducible local rendering](#reproducible-local-rendering)
- [MCP and browser use](#mcp-and-browser-use)
- [Optional external bridges](#optional-external-bridges-not-baseline-requirements)

## Default capability map

| Work | Maintained source | Inspect/export | Boundary |
| --- | --- | --- | --- |
| TUI | Existing framework/code | Project tests, real terminal/PTY, screenshots or recordings | No browser substitute for terminal behaviour |
| Web/dashboard/PWA | Existing components, semantic HTML/CSS, data/query definitions | Configured browser MCP, project tests; accessible table for charts | Visible Brave only; live data/deploy gates unchanged |
| Native/mobile | Target toolkit source and semantic tokens | Actual target build, accessibility tree, simulator/device and screenshots | Host/SDK/device access must exist; a mockup is not runtime evidence |
| Deck default | `.typ` with editable text/vector charts | Typst PDF and PNG pages | Source editable, not PowerPoint editable |
| Required PPTX | Python generator plus `.pptx` native text/shapes/charts/notes | python-pptx reopen; LibreOffice edit/export; page rasterization | Recipient editor/AT compatibility needs its own check |
| Graphics | SVG with editable text/vectors | librsvg PNG; Inkscape for editor round trip if needed | Preserve font licence and text; no script/external-resource SVG from untrusted sources |

Typst and python-pptx are Apache-2.0 and MIT respectively; librsvg is LGPL,
LibreOffice uses MPL/LGPL licensing. Recheck package metadata and embedded asset
licences for distribution. Use local/open-licensed fonts, list exact family and
fallback, and verify output for substitutions. No online font dependency is needed.

## Reproducible local rendering

Run inside the artifact directory. These examples use the dotfiles locked
nixpkgs input; another repository should prefer its own approved dev shell.
No command opens an account, publishes content or launches a browser.

```sh
nix shell --inputs-from "$HOME/.dotfiles" nixpkgs#typst -c \
  typst compile deck.typ deck.pdf
nix shell --inputs-from "$HOME/.dotfiles" nixpkgs#typst -c \
  typst compile --format png deck.typ 'slide-{p}.png'
nix shell --inputs-from "$HOME/.dotfiles" nixpkgs#librsvg -c \
  rsvg-convert --output graphic.png graphic.svg
nix shell --inputs-from "$HOME/.dotfiles" nixpkgs#poppler-utils -c \
  pdftoppm -png -r 120 deck.pdf slide
```

Read every resulting PNG with `read`. PDF file existence and a zero renderer exit
code prove neither visual quality nor accessible reading order. Keep the source
and generation commands; edit one source value and verify the re-export.

For independently authored PPTX generators on this flake:

```sh
nix shell --impure --expr '
  let f = builtins.getFlake (builtins.getEnv "HOME" + "/.dotfiles");
      p = f.inputs.nixpkgs.legacyPackages.${builtins.currentSystem};
  in p.python3.withPackages (ps: [ ps.python-pptx ])
' -c python3 make_deck.py
```

Use `pptx.Presentation`, text boxes and native charts with editable data. Do not
use slide-sized images as the content model. For PDF previews use an isolated
LibreOffice profile so you do not interfere with an open user document:

```sh
profile="$(mktemp -d)"
mkdir -p preview
soffice "-env:UserInstallation=file://$profile" --headless \
  --convert-to pdf --outdir preview deck.pptx
# Verify the export, then remove only the temporary profile you created.
```

LibreOffice headless conversion is not headless browser control. It is a preview,
not proof of PowerPoint fidelity. Reopen the PPTX, edit a text/chart value and
re-export; inspect native shapes and data as well as every rendered page.

For an opaque sRGB colour pair, the bundled helper is a narrow numerical check:

```sh
python3 /absolute/path/to/design-workflow/scripts/contrast.py '#18212b' '#ffffff'
```

It fails below 4.5 by default; use `--minimum 3` only for an applicable large-text
or non-text criterion. It does not compute composited transparency, gradients,
font weight, focus quality or accessibility compliance. Measure rendered states.

## MCP and browser use

Inspect configured MCP tools first. Design roles expose `mcp`, not guessed direct
browser/Figma tool names. Use `mcp({server:"browser"})`, describe the selected
tool, then call its discovered schema. Load `browser-automation` before control.
Bind an owned tab and revalidate identity before mutation. Screenshots may return
only a saved path: use `read` on it. Pass absolute artifact paths to children;
parent image attachments do not automatically follow delegation.

The gateway can reach unrelated services. A role's tool list is not a sandbox.
Never test unapproved effects on a live account. An unavailable browser means
report the missing evidence, not start a headless or isolated browser.

## Optional external bridges, not baseline requirements

Research checked 2026-09-27. Recheck current capabilities, client access, pricing
and terms before connecting. These bridges are not installed or authenticated by
the design workflow. Choose one only when that service is the source of truth.

- **Figma official MCP:** structured node/context, variables and screenshots;
  remote write-to-canvas where the client/seat supports it. Pi access and writes
  are not assumed. Read [access rules](https://developers.figma.com/docs/figma-mcp-server/rate-limits-access/)
  and [write requirements](https://developers.figma.com/docs/figma-mcp-server/write-to-canvas/).
  Remote endpoint `https://mcp.figma.com/mcp`; desktop selection bridge is narrower
  and needs the desktop app. Code output is context to translate into project
  components, not permission to replace the stack. Validate exact file/node and
  read back changes. Do not bypass a client restriction with another identity.
- **Penpot official MCP:** local plugin bridge can create/edit real shapes through
  plugin code; needs a running, connected plugin and matching server. Use the
  [maintained in-repo MCP](https://github.com/penpot/penpot/tree/develop/mcp), not
  the archived standalone repo. Review/pin packages through the supply-chain
  workflow rather than executing an unpinned `npx -y` instruction. Do not disable
  browser security globally to make it connect.
- **Canva design MCP:** `https://mcp.canva.com/mcp` supports authoring/edit/export
  through per-user OAuth. Generation can consume credits; export/resize and assets
  have plan/licence constraints. Its developer-docs MCP is a different product.
  Check [usage policy](https://www.canva.dev/docs/mcp/usage-policy/); do not harvest
  designs/brand kits or rebuild restricted templates outside Canva.
- **Storybook MCP:** use the [official addon](https://storybook.js.org/docs/ai/mcp/overview)
  only in an existing/approved project Storybook. It provides component context
  and optional tests, not a design canvas or whole-app verification. Verify
  framework/test-addon support; keep local endpoints private.

Before optional enablement: identify exact service/project/file, read/write needs,
data allowed to leave the machine, cost and authorization. Obtain credentials or
OAuth approval through the coordinator. Then use the Pi gateway's supported
install/discovery flow, not hand-edited token files. List/describe tools, perform
authorized reads first, and inspect independent persisted state after writes.
A successful connection is not proof of write access, export fidelity or approval.
