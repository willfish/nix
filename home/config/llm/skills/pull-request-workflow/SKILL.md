---
name: pull-request-workflow
description: Pull request preparation. Use for PR titles, descriptions, summaries, Jira links, branch/commit conventions, diagrams, deployment risk notes, CLI demo GIFs, or Slack PR roundups.
---

# Pull Request Workflow

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use this for PR work.

Read:
- `references/prs.md` for the PR overview and defaults.
- `references/pr-title-body.md` for PR titles, descriptions, risk sections, labels, and before-review checks.
- `references/pr-diagrams.md` for Mermaid diagrams in PR descriptions.
- `references/pr-slack-roundups.md` for Slack PR summaries.
- `references/git.md` for branch names, commit format, pre-commit/direnv notes, and Slack PR roundups.
- `references/voice.md` when drafting text that should sound like Will.

Defaults:
- PR titles use `{story}: Imperative description` for story work; when there is no Jira story, use a short descriptive title without a fake ticket key.
- Commits use conventional commit subjects. Put the Jira story key in the body/footer as `Jira: PROJ-123`; for work with no ticket, use `Issue: No ticket/issue`.
- Branch names use `{story}-short-kebab-description`; never add agent/tool prefixes such as `codex/`.
- Link the child story, not only the parent epic.
- Use a checklist in the `What?` section.
- Follow the repository's PR template exactly. Do not invent additional
  top-level sections. Do not narrate routine verification or duplicate CI results.
  Include only reviewer-relevant evidence CI does not provide, or material gaps.
- Include diagrams when they make the change easier to understand; when adding Mermaid, use the `diagramming` skill and its preferred inline Mermaid style.
- Inspect configured MCP tools first. Use GitHub MCP for PR/repo/CI reads when available; use `gh` for operations not exposed by MCP or when exact CLI behaviour is needed.
- For Slack PR roundups, use GitHub MCP or `gh` to gather PRs, then draft one concise line per PR. Use Slack MCP only to read relevant channel context or to post after explicit user approval.
