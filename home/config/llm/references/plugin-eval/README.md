# Plugin Eval Reference

Source: https://github.com/thisdot/plugin-eval
Checked: 2026-06-20
Update trigger: plugin-eval is vendored or removed, Codex plugin APIs change, or quarterly skill-maintenance review.

This directory keeps design/reference material for a `plugin-eval` workflow. It
is not a runnable plugin or CLI checkout in this dotfiles repo.

## What Is Present

- `references/`: design notes for chat-first workflows, result schemas,
  observed usage, benchmark harnesses, and metric-pack manifests.
- `skills/`: reference examples of the skills a real plugin-eval plugin could
  expose.
- `fixtures/`: minimal fixtures retained as examples.

## What Is Not Present

- No `scripts/plugin-eval.js` CLI entrypoint.
- No `.codex-plugin/plugin.json` plugin manifest.
- No package manifest, Node dependencies, or runnable benchmark engine.

Do not tell users to run `plugin-eval` from this directory unless a real CLI has
been vendored or installed elsewhere.

## Local Maintenance Workflow

For this dotfiles harness, use the local static audit first:

```bash
home/config/llm/scripts/audit-skills
```

Use the retained plugin-eval references only when designing a future evaluation
tool or deciding what a richer skill/plugin evaluator should measure.

## Useful References

- [Chat-first workflows](./references/chat-first-workflows.md)
- [Observed usage inputs](./references/observed-usage.md)
- [Technical design](./references/technical-design.md)
- [Evaluation result schema](./references/evaluation-result-schema.md)
- [Metric pack manifest](./references/metric-pack-manifest.md)
