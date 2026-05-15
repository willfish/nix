---
name: pull-request-workflow
description: >
  Use when creating, updating, or summarising pull requests for the trade-tariff
  repos, including title conventions, description structure, Jira linking,
  mermaid diagrams, deployment risk notes, CLI demo GIFs, and Slack PR roundups.
metadata:
  short-description: "PR creation and management for AI project tickets"
---

# Pull Request Workflow

Use this whenever you are preparing or updating a PR in the trade-tariff repositories.

**Read first:**
- `references/prs.md` — Title format (`AI-{story}: Imperative description`), description structure, Jira links, mermaid diagrams, "Have you?" checklist, deployment risks, CLI demo expectations.
- `references/git.md` — Branch naming, commit message format, pre-commit hooks with direnv, how to generate Slack PR roundups.
- `references/voice.md` — When the PR description or Slack summary needs to sound like Will wrote it.

Defaults:
- Every PR title and commit must start with the child story key (`AI-xxx`), not the epic.
- Use checklists in the "What?" section.
- Include diagrams for anything non-trivial. When diagrams are needed, invoke the `diagramming` skill (see `references/diagramming.md` for tool choice, GitHub limitations, and best practices).
- Draft Slack summaries as one concise line per PR in plain language.