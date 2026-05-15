---
name: diagramming
description: >
  Use when creating, reviewing, or improving diagrams (Mermaid, D2, Excalidraw, PlantUML, etc.)
  for PRs, architecture documentation, GitHub issues, design discussions, or any technical writing
  that would benefit from visual explanation. Especially relevant for complex systems, workflows,
  and when preparing content for the trade-tariff stack or AI Jira project.
metadata:
  short-description: "Best practices for technical diagrams across tools and platforms"
---

# Diagramming Skill

Use this skill whenever diagrams are involved in your work.

**Read first:**
- `references/diagramming.md` — Main guide covering tool selection, GitHub Mermaid limitations, diagram quality guidelines, and workflows.
- `references/tool-selection.md` — Decision matrix for choosing between Mermaid, D2, Excalidraw, and committed SVGs.
- `references/mermaid-github-tips.md` — Practical tips and gotchas for using Mermaid directly on GitHub.
- `references/rendering-diagrams.md` — How to actually render Mermaid/D2 to PNG or SVG for verification (using Nix + mermaid-cli or d2).

Key principles:
- Choose the right tool for the audience and longevity of the diagram.
- GitHub's native Mermaid support is convenient but has significant limitations — know when to commit an SVG instead.
- Good diagrams are clear, have a single purpose, and remain readable in both light and dark mode.
- For important architecture, prefer committed SVGs with source files over inline Mermaid.

This skill is referenced by:
- `pull-request-workflow` (when deciding whether to include diagrams in PR descriptions)
- `hmrc-trade-tariff-workflow` (for architecture and design work on the trade-tariff systems)
- `code-review-workflow` (when reviewing diagrams in PRs)