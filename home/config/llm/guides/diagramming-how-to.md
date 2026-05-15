# How to Use the Diagramming Skill

The `diagramming` skill helps you create clear, maintainable technical diagrams and gives consistent feedback when reviewing diagrams in PRs.

## When to Invoke It

Use the `diagramming` skill (or read `references/diagramming.md`) in these situations:

- You are about to create a diagram for a PR, architecture doc, or design discussion.
- You need to decide between inline Mermaid, D2 + SVG, Excalidraw, etc.
- You are reviewing a PR that contains diagrams and want structured feedback.
- You want to improve an existing diagram's clarity or GitHub rendering.

## Recommended Workflow

### Creating a New Diagram

1. **Start by reading the guides**:
   ```text
   Use the diagramming skill. Read references/tool-selection.md and references/diagramming.md first.
   ```

2. Choose the right tool using the decision matrix in `tool-selection.md`.

3. For anything that will live in architecture documentation or be referenced across multiple PRs:
   - Prefer **D2** → commit both the `.d2` source and the rendered `.svg`.
   - Store sources in `docs/diagrams/` or `docs/architecture/`.

4. For small, PR-specific flow or sequence diagrams:
   - Inline Mermaid is acceptable, but follow the tips in `mermaid-github-tips.md`.

5. Always test rendering in both light and dark mode before submitting.

### Reviewing Diagrams in a PR

When a PR contains diagrams:

```text
Use the diagramming skill with references/diagram-review-checklist.md to review the diagrams.
```

Go through the checklist and leave specific, actionable feedback (e.g., “The subgraph nesting is causing layout issues on GitHub — consider splitting into two diagrams” or “This would be clearer as a D2 diagram committed as SVG”).

## Available References

| File                              | Purpose                                      |
|-----------------------------------|----------------------------------------------|
| `references/diagramming.md`       | Main guide + best practices                  |
| `references/tool-selection.md`    | Decision matrix for choosing a tool          |
| `references/mermaid-github-tips.md` | GitHub-specific Mermaid limitations & fixes |
| `references/diagram-review-checklist.md` | Structured checklist for code review     |

## Quick Commands (for reference)

- Generate D2 diagram: `d2 input.d2 output.svg`
- Generate Mermaid diagram (CLI): `mmdc -i input.mmd -o output.svg`
- Excalidraw: Use the web app or desktop app and export SVG + JSON source.

This skill is available in Grok, Codex, and (when enabled) Gemini. The content is the same across all tools because it lives in the single-source `llm/guides/` directory.