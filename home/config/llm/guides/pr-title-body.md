# PR title and body

Before drafting prose, read `~/.agents/guides/documentation-relevance.md`.

Use the repository's PR template and title conventions. Link the child issue
when one exists; do not invent a ticket. Use a concise imperative title.
Describe what changes, why it is needed, and material risks. Keep the description
self-contained and omit routine CI results and implementation diaries.

## Risk and approval

Follow the repository's risk criteria, labels and approval gates. Do not assume
that passing tests authorizes a merge. If the repository has no policy, explain
risk concretely rather than inventing a mandatory label:

- Low: documentation, tests, or behaviour-preserving refactors.
- Medium: API, indexing, deployment or infrastructure behaviour changes.
- High: destructive migrations, credential handling, legally significant data,
  irreversible infrastructure changes, or new service boundaries.

High-risk changes require explicit approval from the responsible maintainers.
Never downgrade risk to make a change easier to merge.

## Before requesting review

Run full relevant checks in the actual worktree and review the diff. Verify that
the title, body, issue link and any required risk labels match the final change.
Include material coverage gaps or non-CI evidence in the template's existing
sections. Do not add headings solely to report routine checks.
