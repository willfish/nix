# Rendering Diagrams to PNG / SVG

This guide explains how to turn Mermaid (or D2) source into actual image files for preview and verification.

## Mermaid CLI (Recommended for README diagrams)

The best tool for rendering Mermaid is the official CLI: `@mermaid-js/mermaid-cli`

### In this repo's dev shell (`nix develop`)

- `d2` is always available (excellent for architecture diagrams)
- `nodejs` is available
- On Linux: `mmdc` command is provided (wrapped with Chromium)
- On macOS (Apple Silicon): use `npx @mermaid-js/mermaid-cli`

Example usage after `nix develop`:

```bash
# Linux
mmdc -i diagram.mmd -o diagram.png --scale 2

# macOS / general
npx @mermaid-js/mermaid-cli -i diagram.mmd -o diagram.png --scale 2 --backgroundColor "#ffffff"
```

The repo includes a helper script for the README diagrams:

```bash
./docs/diagrams/render-readme-diagrams.sh
```

### Common flags

- `-i input.mmd` — input Mermaid file
- `-o output.png` (or `.svg`)
- `--scale 2` — higher resolution (very useful for READMEs)
- `--width 1200` / `--height 800` — control canvas size
- `--backgroundColor transparent` or `#ffffff`

### Recommended workflow for README verification

1. Extract the Mermaid block from the README into a `.mmd` file.
2. Render with `--scale 2` or `--scale 3`.
3. Open the PNG and inspect:
   - Text readability (especially in dark mode simulation)
   - Overlapping elements
   - Subgraph layout
   - Overall density

### Pro tips

- Always render at 2× scale for GitHub (GitHub compresses images).
- Test both light and dark backgrounds:
  ```bash
  mmdc -i diagram.mmd -o diagram-light.png --backgroundColor "#ffffff"
  mmdc -i diagram.mmd -o diagram-dark.png --backgroundColor "#1e293b"
  ```
- For very wide diagrams, increase `--width`.

## Mermaid Syntax Gotchas That Break Rendering

### Parentheses in Node Labels

One of the most common causes of "Parse error" on GitHub is unescaped parentheses inside node labels.

**Broken example:**
```mermaid
HM_LINUX[william (Linux)]
HM_DARWIN[william (macOS)]
```

**Error you will see:**
> Expecting 'SQE', 'DOUBLECIRCLEEND', ... got 'PS'

**Recommended fix (HTML entities):**
```mermaid
HM_LINUX["william &#40;Linux&#41;"]
HM_DARWIN["william &#40;macOS&#41;"]
```

This renders cleanly as **william (Linux)** without any visible backslashes.

**Why backslash escaping is not ideal:**
While `\( \)` prevents the parse error, the backslashes often remain visible in the final rendered diagram on GitHub.

**Better rule of thumb:**
For special characters in labels (`( ) [ ] & # : ;`), prefer **HTML entities** over backslash escaping when the diagram will be viewed on GitHub. HTML entities give the cleanest visual result.

Other characters that often need escaping or quoting:
- `&`
- `#`
- `:`
- `;`

When in doubt, quote the label and escape problematic characters. This is especially important for diagrams that will be rendered on GitHub.

## D2 Rendering (for higher quality architecture diagrams)

```bash
# Using Nix
nix run nixpkgs#d2 -- input.d2 output.svg
nix run nixpkgs#d2 -- --layout elk input.d2 output.png   # better layout engine

# Dark mode
d2 --dark-theme 200 input.d2 output-dark.png
```

D2 generally produces much cleaner results than Mermaid for complex system diagrams and has excellent dark mode support.

## When to Use Which

- Quick README Mermaid diagrams → Use `mermaid-cli` + PNG for verification
- Serious architecture → Use **D2** and commit the `.svg` (or both `.d2` + `.svg`)
- Exploratory diagrams → Excalidraw → export SVG

## Adding Rendering to Your Workflow

When working on diagrams for this repo:

1. Create/edit the Mermaid or D2 source.
2. Run the appropriate render command (using the commands above).
3. Open the PNG/SVG and apply the **Diagram Review Checklist**.
4. Only commit once the rendered image looks clean.

This is much more reliable than trusting how the diagram looks in your editor or on GitHub's live preview.

### Rendering the README diagrams (this repo)

This repository includes a helper script:

```bash
./docs/diagrams/render-readme-diagrams.sh
```

It will generate:
- `docs/diagrams/rendered/architecture.png`
- `docs/diagrams/rendered/repo-structure.png`

Open both and apply the checklist from `references/diagram-review-checklist.md`.