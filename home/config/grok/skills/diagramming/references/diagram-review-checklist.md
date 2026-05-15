# Diagram Review Checklist

Use this checklist when reviewing diagrams in a PR or design document.

## Clarity & Purpose

- [ ] The diagram has a single, clear purpose (it doesn't try to show everything).
- [ ] The title or caption immediately tells the reader what the diagram is about.
- [ ] There are no more than ~8–10 primary nodes (split complex diagrams into multiple views if needed).
- [ ] The direction of flow is consistent and natural (usually top-to-bottom or left-to-right).

## Readability

- [ ] Text is large enough and high contrast (especially important for dark mode viewers).
- [ ] Node labels are short and scannable (avoid long sentences inside boxes).
- [ ] Relationships/lines are clearly labeled where necessary.
- [ ] Colors (if used) have meaning and are explained in a legend.

## Technical Quality

- [ ] If using Mermaid on GitHub: the diagram renders cleanly without overlapping text or broken subgraphs.
- [ ] If a committed SVG: both the source file (`.d2`, `.excalidraw`, etc.) **and** the `.svg` are included in the PR.
- [ ] The diagram is stored in a logical location (`docs/architecture/`, `docs/diagrams/`, etc.) rather than scattered.
- [ ] The diagram will still make sense 6–12 months from now (avoid referencing ephemeral details like specific PR numbers or temporary service names).

## Integration with the Change

- [ ] The diagram actually helps explain the "Why?" or "How?" of the change (it's not just decorative).
- [ ] The diagram is referenced from the PR description or relevant documentation.
- [ ] If the diagram shows architecture, it is consistent with the actual code being changed in this PR.

## Red Flags

- The diagram is so complex that the reviewer needs to ask the author to explain it.
- The diagram duplicates information that is already clearly expressed in code or text.
- The diagram will become stale quickly (e.g., it hard-codes many specific class names or configuration values that are likely to change).

**Strongly prefer** committed, high-quality SVGs (generated from D2 or Excalidraw) for anything that will live in architecture documentation or be referenced across multiple PRs. Inline Mermaid is acceptable for small, PR-specific sequence or flow diagrams.