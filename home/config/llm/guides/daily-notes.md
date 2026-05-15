# Daily Notes

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
2. **Summarise completed work** in "What I did yesterday":
   - PRs merged/reviewed (reference by number, not full URL)
   - Meetings and discussions (topic only, not attendees)
   - Development work (feature names, version numbers)
3. **Leave "What I plan to do today" blank** unless the user specifies tasks
4. **Keep entries concise** — one line per item, no sub-bullets

## Style

- Use PR numbers not full URLs: `#2742` not `https://github.com/.../pull/2742`
- Group related PRs: "PRs for AI project: #2742, #2743, #1091"
- Name features/tools by their user-facing name, not internal codenames
- Version releases: "mux v0.2.0 release: feature list"
