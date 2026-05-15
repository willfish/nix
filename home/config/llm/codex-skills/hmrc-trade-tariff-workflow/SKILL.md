---
name: hmrc-trade-tariff-workflow
description: Use when working on HMRC trade-tariff repositories, Jira AI project issues, PRs, code reviews, RSpec tests, trade-tariff frontend authentication, epic/story writing, Git conventions, or Slack PR summaries for Will's trade-tariff work.
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
- Jira project key is `AI`.
- Commit and PR titles use `AI-{story}: Imperative description`.
- Prefer `direnv exec <repo> <command>` or run commands from the activated repo shell.
- Use `gh` for GitHub repos, PRs, and CI rather than browser scraping.
- Do not send Slack messages directly; draft for review when Slack tooling is available.
