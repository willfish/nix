---
name: jira-workflow
description: Use when working with Jira issues for Will, especially transformuk.atlassian.net, HMRC or AI project tickets, Jira API v3, ADF descriptions/comments, epics, stories, issue transitions, or creating/updating Jira tickets.
---

# Jira Workflow

Use this for Jira work.

Read `references/jira.md` before using the Jira API or changing issues. It contains the instance, auth pattern, API v3 endpoints, project selection rules, project IDs, hierarchy, transitions, and ADF examples.

Read `references/epics-and-stories.md` when writing or reviewing epic/story descriptions. Keep business narrative first and technical detail in collapsible implementation sections.

Defaults:
- Jira instance: `transformuk.atlassian.net`.
- Project key: use `HMRC` for HMRC trade-tariff or other non-AI work; use `AI` only when the work is explicitly for the AI project.
- Use API v3 only.
- Source credentials from `~/.env`; do not print tokens.
- Use ADF for descriptions and comments.
