---
name: code-review-workflow
description: >
  Use when reviewing PRs in the trade-tariff repos, writing review comments,
  responding to review feedback, or applying Will's specific code review tone
  and priorities (correctness, edge cases, performance, security, tests, scope).
metadata:
  short-description: "Code review workflow and tone for trade-tariff PRs"
---

# Code Review Workflow

Use this skill for reviewing pull requests or writing review responses.

**Read first:**
- `references/reviews.md` — Tone guidelines (sound like a human colleague, lead with the biggest issue, use bold for key points), review priorities, and good vs bad examples.
- `references/voice.md` — When the review comment needs to sound like Will wrote it.
- Use the `diagramming` skill when the PR contains diagrams (see `references/diagramming.md` and `references/diagram-review-checklist.md`).

Key principles:
- Lead with the biggest issue.
- Use `--comment` rather than `--request-changes` unless explicitly asked.
- Prioritise: correctness → edge cases → performance → security → tests → scope.
- Keep comments short, concrete, and human-sounding.
- End on something positive when the overall approach is sound.