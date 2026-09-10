---
name: skill-evaluation
description: Skill harness evaluation and maintenance. Use for auditing skills, checking token/disclosure drift, running audit-skills, reviewing plugin-eval references, or planning richer skill/plugin evaluation.
disable-model-invocation: true
---

# Skill Evaluation

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use this for local skill-harness maintenance and evaluation.

## Default Workflow

1. Run the static audit first:

```bash
home/config/llm/scripts/audit-skills
```

2. Treat audit findings as local maintenance work:

- Fix missing skill deployment entries or stale skill names immediately.
- Fix missing `references/*.md` links before editing content.
- Add contents sections to long references or split them by branch.
- Review trigger overlap before widening descriptions.
- Check freshness metadata when external guidance is involved.

3. Use plugin-eval references only as design material unless a real
   `plugin-eval` CLI is installed or vendored:

- `references/plugin-eval.md` for what is retained locally.
- `references/plugin-eval-chat-first-workflows.md` for chat-first workflow design.
- `references/plugin-eval-technical-design.md` for evaluator architecture.
- `references/plugin-eval-observed-usage.md` for observed usage input shapes.
- `references/plugin-eval-evaluation-result-schema.md` for result JSON shape.

## Completion

Before claiming harness maintenance is complete, run:

```bash
home/config/llm/scripts/audit-skills
nix build .#homeConfigurations.william-linux.activationPackage --dry-run
```
