# Git conventions

Before drafting prose, read `~/.agents/guides/documentation-relevance.md`.

## Commits and branches

Use `<type>(optional-scope): short imperative description`. When a ticket
exists, put the child ticket key in the body as `Jira: PROJ-123`, never in the
commit subject. Explain what changes and why, not the diff.

Follow repository branch conventions. Otherwise use a short descriptive name
without an agent or tool prefix. Keep dependent branches based on their parent.
Do not rewrite pushed commits without authorization. Run full relevant checks
and review the diff before committing; use the checkout's direnv environment.

## Slack PR sharing

For a requested roundup, identify the repositories in scope and list the user's
open PRs with short plain-language summaries, ordered by repository and PR number.
Do not silently assume an organization or include unrelated repositories.

Inspect configured MCP tools first. Use GitHub MCP for supported reads and `gh`
only when the required query is unsupported or checkout context matters:

```bash
gh pr list --repo OWNER/REPO --state open --author @me --json url,title
```

Draft one line per PR, with its URL and a short summary rather than a technical
title. Posting to Slack requires explicit user approval; a drafting request does
not authorize publication.
