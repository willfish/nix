# Choosing a Diagramming Tool

This guide helps you decide which tool to use depending on the context.

## Quick Decision Matrix

| Situation                              | Recommended Tool          | Output          | Why |
|----------------------------------------|---------------------------|-----------------|-----|
| Quick diagram inside a PR description | Mermaid (inline)          | Markdown        | Fastest, no extra files |
| Architecture diagram for a PR         | D2 → committed SVG        | SVG + .d2       | Excellent quality + source control |
| Exploratory / hand-drawn feel         | Excalidraw                | SVG             | Fun, great for early design |
| Diagram that must survive light/dark  | D2 or Excalidraw + SVG    | SVG             | Consistent rendering |
| Long-term architecture docs           | D2                        | SVG + .d2       | Best for complex layered systems |
| Sequence diagram with many actors     | Mermaid or PlantUML       | Markdown / SVG  | Mermaid is usually sufficient |
| Need perfect control over layout      | D2 or draw.io             | SVG             | Most precise positioning |

## Detailed Guidance

### Use Mermaid when...
- The diagram is small-medium
- It lives only in one PR or issue
- You want zero friction
- You're okay with occasional GitHub rendering weirdness

### Use D2 when...
- You need high-quality, professional-looking architecture diagrams
- You want excellent dark mode support out of the box
- You're willing to commit both source (`.d2`) and rendered (`.svg`)
- The diagram has multiple layers or complex relationships

### Use Excalidraw when...
- You're in the early "figuring it out" phase
- You want a more human, less corporate feel
- You're collaborating with people who prefer sketch-style diagrams

### Always commit an SVG when...
- The diagram appears in the main README or architecture docs
- Multiple people will reference it over time
- You care about consistent rendering across tools and themes
- The diagram is part of an Architecture Decision Record (ADR)

## Recommended Workflow for Trade-Tariff Work

1. Start in **D2** for any non-trivial architecture change.
2. Export a clean SVG.
3. Commit both `architecture/foo.d2` and `architecture/foo.svg`.
4. Reference the `.svg` in the PR description.
5. If the diagram is small and only relevant to this PR, fall back to inline Mermaid for speed.

This approach gives you both speed when you need it and quality when it matters.