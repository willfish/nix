# Pull Request Guidelines

## Title format

```
{story}: Short imperative description
```

- Use the Jira **story** key for story work, or `BAU` when there is no Jira story
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

**Important:** The PR description must be self-contained. Do not reference other PRs, previous work, or external context ("see the backend PR", "following the same pattern as...", "this builds on PR #xxxx", etc.). Describe why the change was made and exactly what was changed so the description stands alone.

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

## Risk rating criteria

Use the repository's `.github/pull_request_template.md` as the canonical detailed decision tree. If the repo does not have one, use these trade-tariff defaults:

### 🟢 Green / `low-risk`

Low deployment or review risk. Good to go with standard review.

Typical examples:

- Dependency bumps with no API changes, especially minor or patch updates
- Copy or content changes in GOV.UK-style UI components
- Read-only observability changes such as CloudWatch alarms or dashboards
- Tests or coverage improvements with no production code changes
- Purely additive config or environment variables with safe defaults
- Refactors with full test coverage and no behaviour change
- Terraform formatting or variable renaming with no resource recreation
- Non-destructive, time-delayed S3 lifecycle rule additions

### 🟠 Amber / `medium-risk`

Medium deployment or review risk. Socialise with the team before merging.

Typical examples:

- Commodity code lookup, measure type, declarable goods, quota, or duty calculation changes
- OpenSearch indexing changes, including mappings, synonyms, or stop words
- New or modified API endpoints consumed by backend, frontend, admin, or external clients
- Feature flag changes that affect live user journeys
- Infrastructure changes to networking, security groups, or IAM permissions, even if tested in non-production first
- Terraform changes that cause resource replacement
- CI/CD pipeline or deployment ordering changes
- S3 bucket policy or access-control changes
- Removing or deprecating an endpoint or field that may still be consumed

### 🔴 Red / `high-risk`

High deployment or review risk. Requires explicit approval from Thor or Neil before merging.

Typical examples:

- Dangerous database migrations, especially destructive changes such as dropping columns, tables, or indexes
- Changes to how measures, conditions, or footnotes are processed or surfaced
- Synchronisation changes for CDS or HMRC upstream data feeds
- Production AWS infrastructure changes that cannot be easily rolled back, including RDS parameter groups, KMS key policy, or resource removal
- Secrets rotation or changes to how credentials are stored, scoped, or accessed
- Changes affecting trader-facing regulatory content or legally significant data, including trade remedies, licensing, or prohibitions
- Significant architectural shifts such as new service boundaries or queue/event topology changes

## PR labels

When opening a PR in a `trade-tariff` repo, always apply exactly one risk label:

- `low-risk` for 🟢 changes
- `medium-risk` for 🟠 changes
- `high-risk` for 🔴 changes

The GitHub label must match the **Risk level** in the PR description. Apply it as part of PR creation or immediately after opening the PR:

```bash
gh pr edit <pr-number-or-url> --add-label low-risk
```

Do not apply more than one risk label to the same PR. If the risk rating changes during review, remove the old risk label and add the new one.

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
- Ensure the matching `low-risk`, `medium-risk`, or `high-risk` label is applied
- Check that the PR title and description accurately reflect the changes
