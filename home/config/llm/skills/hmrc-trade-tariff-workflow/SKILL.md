---
name: hmrc-trade-tariff-workflow
description: Use when working on HMRC trade-tariff repositories, Jira HMRC or AI project issues, PRs, code reviews, RSpec tests, trade-tariff frontend authentication, epic/story writing, Git conventions, or Slack PR summaries for Will's trade-tariff work.
---

# HMRC Trade Tariff Workflow

Use this skill for Will's HMRC trade-tariff work.

Reference selection:
- Jira API, ADF, issue creation/update, transitions: read `references/jira.md`.
- Epics and story descriptions: read `references/epics-and-stories.md`.
- Branches, commit messages, and Slack PR roundups: read `references/git.md`.
- Pull request title/body conventions: read `references/prs.md`.
- Code review tone and priorities: read `references/reviews.md`.
- Test strategy and RSpec conventions: read `references/testing.md`, then `references/rspec.md` for detailed syntax.
- Frontend local auth and request patterns: read `references/trade-tariff-frontend.md`.

Defaults:
- Jira project key is `HMRC` for HMRC trade-tariff or other non-AI work; use `AI` only when the work is explicitly for the AI project.
- Commit and PR titles use `{story}: Imperative description`, where `{story}` is the child Jira story key or `BAU` when there is no Jira story.
- Branch names use `{story}-short-kebab-description`; never add agent/tool prefixes such as `codex/`.
- Prefer `direnv exec <repo> <command>` or run commands from the activated repo shell.
- Use `gh` for GitHub repos, PRs, and CI rather than browser scraping.
- Do not send Slack messages directly; draft for review when Slack tooling is available.
