# Pull Request Guidelines

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use this as the PR overview. Load only the branch reference needed for the task.

## Branch References

- `references/pr-title-body.md` for PR titles, descriptions, risk sections,
  labels, and before-review checks.
- `references/pr-diagrams.md` for Mermaid diagrams in PR descriptions.
- `references/pr-slack-roundups.md` for concise Slack PR summaries.

## Defaults

- PR titles use `{story}: Imperative description` for story work.
- When there is no Jira story, use a short descriptive title without a fake
  ticket key.
- Link the specific child story, not only the parent epic.
- Use a checklist in the `What?` section.
- Include a Risk section on every PR.
- Follow the repository's risk-label policy and required approval gates.
- For CLI behavior changes, include an animated demo GIF and use the
  `terminal-demos` skill.
