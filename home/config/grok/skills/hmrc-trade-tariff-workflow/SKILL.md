---
name: hmrc-trade-tariff-workflow
description: >
  Use when working on HMRC trade-tariff repositories (backend, admin, frontend),
  the AI Jira project on transformuk.atlassian.net, PR creation, code reviews,
  RSpec testing, trade-tariff frontend local authentication and testing,
  writing epics/stories, Git conventions, or Slack PR summaries.
metadata:
  short-description: "Main router for Will's trade-tariff + AI Jira project work"
---

# HMRC Trade Tariff Workflow

Primary skill for Will's work on the trade-tariff stack and the AI Jira project.

This skill routes you to the detailed guides. Read the relevant reference(s) before starting work:

- **Jira work** (API v3, ADF descriptions, transitions, auth via `~/.env`): `references/jira.md`
- **Epics and stories** (business-first writing style, ADF panels, collapsible implementation details): `references/epics-and-stories.md`
- **Pull requests** (title format `AI-{story}:`, description structure, mermaid diagrams, CLI demos): `references/prs.md`
- **Git & branches** (commit messages, branch naming, Slack PR roundups): `references/git.md`
- **Code reviews** (tone, priorities, what to look for): `references/reviews.md`
- **RSpec & testing** (conventions, when to write tests, trade-tariff patterns): `references/rspec.md` and `references/testing.md`
- **Trade tariff frontend** (local dev auth, Python testing patterns): `references/trade-tariff-frontend.md`

Defaults:
- Jira project key is `AI` on `transformuk.atlassian.net`.
- All PR/commit titles use the `AI-{story}` prefix.
- Prefer `direnv exec <repo> <command>` when working in trade-tariff repos.
- Draft Slack messages for review rather than sending directly when possible.

Use the more specific skills (`jira-workflow`, `pull-request-workflow`, `will-voice`, `code-review-workflow`, `rspec-testing`) when you need deeper focus on one area.