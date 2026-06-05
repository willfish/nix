---
name: pull-request-workflow
description: Use when creating, updating, summarising, or preparing pull requests for Will, including PR titles, PR descriptions, Jira links, mermaid diagrams, deployment risk notes, CLI demo GIF expectations, GitHub CLI usage, branch naming, commits, and Slack PR roundups.
---

# Pull Request Workflow

Use this for PR work.

Read:
- `references/prs.md` for PR title/body structure.
- `references/git.md` for branch names, commit format, pre-commit/direnv notes, and Slack PR roundups.
- `references/voice.md` when drafting text that should sound like Will.

Defaults:
- PR titles and commits use `{story}: Imperative description`, where `{story}` is the child Jira story key or `BAU` when there is no Jira story.
- Branch names use `{story}-short-kebab-description`; never add agent/tool prefixes such as `codex/`.
- Link the child story, not only the parent epic.
- Use a checklist in the `What?` section.
- Include diagrams for non-trivial changes; when adding Mermaid, use the `diagramming` skill and its preferred inline Mermaid style.
- Use `gh` for GitHub operations.
- For Slack PR roundups, draft one concise line per PR with plain-language summaries.
