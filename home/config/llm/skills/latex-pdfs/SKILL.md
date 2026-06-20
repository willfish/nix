---
name: latex-pdfs
description: LaTeX PDF generation. Use for professional PDFs, pdflatex, nix-shell TeX environments, document templates, missing packages, or PDF troubleshooting.
---

# LaTeX PDFs

Use LaTeX through `nix-shell` for portable PDF generation.

Rules:
- Run `pdflatex` twice so references and links resolve.
- Use the standard professional article template from the reference unless the user supplies a design.
- For missing `.sty` files, resolve the package through TeX Live rather than installing ad hoc local files.
- Save durable PDFs where the user asks; use `/tmp` for throwaway output.

Read `references/pdfs.md` for commands, package discovery, and the template.
