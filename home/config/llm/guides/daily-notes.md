# Daily Notes

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

## Location and structure

- Notes directory: `~/Notes/`
- Daily file: `~/Notes/YYYY-MM-DD/today.md`
- Template: `~/Notes/templates/today.md`

## Template format

```markdown
# YYYY-MM-DD

What I did yesterday

- Item 1
- Item 2

What I plan to do today

-
-
-

Blocked by

-

Team Blockages and follow up
```

## Populating notes

When asked to populate daily notes:

1. **Read the previous day's file** (`~/Notes/YYYY-MM-DD/today.md` for yesterday's date) to see what was planned and any PR links
2. **Gather recent work context** if the user asks you to infer it:
   - Use GitHub MCP for PRs/issues/commits when available; use `gh` as fallback.
   - Use Slack MCP for relevant work-channel context only when useful. Do not post messages.
3. **Summarise completed work** in "What I did yesterday":
   - PRs merged/reviewed (reference by number, not full URL)
   - Meetings and discussions (topic only, not attendees)
   - Development work (feature names, version numbers)
4. **Leave "What I plan to do today" blank** unless the user specifies tasks
5. **Keep entries concise** — one line per item, no sub-bullets

## Style

- Use PR numbers not full URLs: `#2742` not `https://github.com/.../pull/2742`
- Group related PRs: "PRs for AI project: #2742, #2743, #1091"
- Name features/tools by their user-facing name, not internal codenames
- Version releases: "mux v0.2.0 release: feature list"
