---
name: jira-workflow
description: >
  Use when working with Jira on the AI project (transformuk.atlassian.net),
  creating/updating issues, writing ADF descriptions or comments, transitioning
  tickets, fetching epics/stories, or using the Jira API v3.
metadata:
  short-description: "Jira AI project workflows (API v3, ADF, epics/stories)"
---

# Jira Workflow (AI Project)

Use this for all work involving the AI Jira project on `transformuk.atlassian.net`.

**Read first:**
- `references/jira.md` — Full API v3 guide, authentication via `~/.env`, search, create/update/transition, ADF formatting, known quirks and workarounds.
- `references/epics-and-stories.md` — How to write epics and stories in the required business-first style with proper ADF panels and collapsible implementation details.

Key facts:
- Project key: `AI`
- Use API v3 only (v2 returns 410).
- Auth: Basic via `JIRA_EMAIL` and `JIRA_API_TOKEN` from `~/.env`.
- Issue hierarchy: Epic → Story/Task/Bug/Feature → Subtask.
- Always include a project filter (`project = AI`) in JQL searches.