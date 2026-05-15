# Generating PDFs with LaTeX

Use LaTeX via `nix-shell` for PDF generation. This avoids needing a permanent install and works from any directory.

## Quick command

```bash
nix-shell -p '(pkgs.texlive.combine { inherit (pkgs.texlive) scheme-small enumitem titlesec fancyhdr parskip booktabs tools collection-fontsrecommended hyperref xcolor; })' \
  --run "pdflatex -interaction=nonstopmode document.tex && pdflatex -interaction=nonstopmode document.tex"
```

Run `pdflatex` twice so that cross-references, table of contents, and hyperlinks resolve correctly.

## Permanent install

texlive is configured in `~/.dotfiles/home/user/packages.nix` using the same `texlive.combine` expression. Apply with `home-manager switch`.

## LaTeX document template

Use this as a starting point for professional-looking documents:

```latex
\documentclass[11pt,a4paper]{article}
\usepackage[utf8]{inputenc}
\usepackage[T1]{fontenc}
\usepackage{lmodern}
\usepackage[margin=2.5cm]{geometry}
\usepackage{hyperref}
\usepackage{xcolor}
\usepackage{booktabs}
\usepackage{tabularx}
\usepackage{enumitem}
\usepackage{titlesec}
\usepackage{fancyhdr}
\usepackage{parskip}

\definecolor{accent}{HTML}{2B6CB0}
\hypersetup{colorlinks=true, linkcolor=accent, urlcolor=accent}
\titleformat{\section}{\Large\bfseries\color{accent}}{}{0em}{}[\titlerule]
\titleformat{\subsection}{\large\bfseries}{}{0em}{}

\pagestyle{fancy}
\fancyhf{}
\fancyhead[L]{\small\color{gray}Document Title}
\fancyhead[R]{\small\color{gray}\thepage}
\renewcommand{\headrulewidth}{0.4pt}

\begin{document}
% content here
\end{document}
```

## Adding texlive packages

If `pdflatex` fails with `File 'foo.sty' not found`, add the missing package to the `texlive.combine` expression. Find the package name with:

```bash
nix-shell -p texlive.combined.scheme-full --run "kpsewhich foo.sty"
```

Then check which texlive package provides it. Common extras: `memoir`, `beamer`, `listings`, `minted`, `tikz` (in `pgf`), `tcolorbox`.

## Output location

By default, save generated PDFs to `~/Pictures/` alongside diagrams, or to `/tmp/` for throwaway documents.
