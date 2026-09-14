# PR title and body

Before drafting prose, read `~/.agents/guides/documentation-relevance.md`.

Use the repository's PR template and title conventions. Link the child issue
when one exists; do not invent a ticket. Use a concise imperative title.
Describe what changes, why it is needed, and material risks. Keep the description
self-contained and omit implementation diaries. Never include verification
details in PR bodies: no test commands, results, counts, CI status, build logs,
manual checks, screenshots or benchmarks. Report evidence and blockers in the
conversation instead. Omit template verification fields unless the user
explicitly requests them.

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

Check the risk-label workflow before creating a PR. Include its required risk
marker in the initial body rather than relying on a label applied afterwards.
If auto-merge still sees an earlier failed risk-label run after newer runs pass,
rerun that failed run with user authorization and verify the result.

## Before requesting review

Run full relevant checks in the actual worktree and review the diff. Verify that
the title, body, issue link and any required risk labels match the final change.
Keep material risks in the body, but report coverage gaps and verification
evidence in the conversation. Do not add verification sections or move their
contents into other sections.
