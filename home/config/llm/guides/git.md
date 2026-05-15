# Git Conventions

## Commit message format

```
AI-{story}: Short imperative description
```

- Prefix with the Jira story key, not the epic key
- Use imperative mood: "Add", "Fix", "Remove", "Restrict", "Wire" — not "Added", "Fixes", "Removing"
- Keep to a single line unless there's essential context that warrants a body
- The message should describe the *what*, not the *how* — the diff shows the how

## Examples

```
AI-141: Add AdminConfiguration model, API, seed task and new suggestion types
AI-142: Wire AdminConfiguration into runtime services and workers
AI-143: Add admin configuration management UI
AI-144: Remove optimised search path from V2 public API
AI-151: Restrict Labels UI to technical operators only
AI-136: Add exhaustive navigation spec coverage for all roles and services
AI-136: Fix OTT Admin sub-navigation not showing on XI root path
```

## Branch conventions

- Branch names: `AI-{story}-short-kebab-description` (e.g. `AI-137-admin-configuration-ui`)
- One squashed commit per story/PR where possible
- When squashing, use `git rebase -i` or `git reset --soft` — not `git commit --amend` on pushed commits
- Keep dependent branches rebased on their parent branch after squashing

## Pre-commit hooks

These repos use pre-commit hooks (rubocop, trufflehog, trailing whitespace, etc.). If a commit is made from outside the repo's direnv shell, wrap it:

```bash
direnv exec /path/to/repo git commit -m "message"
```

## Slack PR sharing

When asked to generate a PR summary for Slack (e.g. "give me my open PRs for Slack", "PR roundup"), list all open PRs authored by the user across all trade-tariff repos with short plain-language summaries.

### Format

```
- <PR URL> <summary in as few words as possible>
```

- One line per PR, no markdown formatting (Slack auto-unfurls the links)
- Summary should be plain language, not the PR title — strip the story prefix and rephrase if the title is too technical
- Keep summaries to ~5-8 words max
- Order by repo, then by PR number ascending (base dependency first)

### How to gather

```bash
# Run across all relevant repos
cd /path/to/trade-tariff-backend && gh pr list --state open --author @me --json url,title
cd /path/to/trade-tariff-admin && gh pr list --state open --author @me --json url,title
cd /path/to/trade-tariff-frontend && gh pr list --state open --author @me --json url,title
```

Known repos (check whichever exist locally under `~/Repositories/hmrc/`):
- `trade-tariff-backend`
- `trade-tariff-admin`
- `trade-tariff-frontend`
