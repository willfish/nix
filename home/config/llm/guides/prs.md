# Pull Request Guidelines

## Title format

```
AI-{story}: Short imperative description
```

- Prefix with the Jira **story** key, not the epic key
- Use imperative mood: "Add", "Fix", "Remove" — not "Added", "Fixes"
- Keep it concise but descriptive

## Description structure

```markdown
### Jira link

[AI-141](https://transformuk.atlassian.net/browse/AI-141)

### What?

- [x] Add user authentication endpoint
- [x] Update session handling middleware
- [x] Add rate limiting to login route

### Why?

The reason for this change — business context or technical motivation.

### Risk

**Risk level:** 🟢

**Reason for rating:** Refactor only with full test coverage and no behaviour change.

```

Use a **checklist** in the What section to enumerate changes. Link to the specific **child story** — not the parent epic.

### Diagrams for non-trivial changes

For PRs that are part of a larger feature or have complex flow, add a mermaid diagram directly below the Jira link to show where this PR fits in.

Place the diagram between the Jira link and the What section:

    ### Jira link

    [AI-154](https://transformuk.atlassian.net/browse/AI-154)

    ```mermaid
    flowchart TB
        A[User action] --> B[This PR's scope]
        B --> C{Decision point}
        C -->|Yes| D[Outcome A]
        C -->|No| E[Outcome B]
    ```

    ### What?

Keep diagrams simple — show the high-level flow, not implementation details. Reviewers should understand the context at a glance.

### Optional sections

Add these when relevant:

```markdown
### Have you?

- [ ] Added documentation for new APIs
- [ ] Added new environment variables to all environments
```

The **Risk** section (above) is now expected on every PR. Use the full decision tree in `.github/pull_request_template.md` when choosing 🟢 / 🟠 / 🔴.

## CLI tool PRs

When the PR adds or changes CLI behaviour, include an animated demo GIF showing the feature in action. See the `terminal-demos.md` guide for how to create these.

```markdown
### Demo

![feature demo](demo.gif)
```

## Before requesting review

- Rebase on the target branch if needed
- Ensure CI is green
- Self-review the diff for obvious issues
- Fill in the **Risk** section with the correct 🟢 / 🟠 / 🔴 rating using the criteria in `.github/pull_request_template.md`
- Check that the PR title and description accurately reflect the changes
