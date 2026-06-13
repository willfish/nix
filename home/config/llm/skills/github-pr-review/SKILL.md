---
name: github-pr-review
description: Use when reviewing GitHub pull requests, posting GitHub review comments, adding GitHub code suggestions, or using gh CLI to submit APPROVE, COMMENT, or REQUEST_CHANGES reviews. Requires drafting comments first, getting explicit user approval, and submitting comments as one pending GitHub review.
---

# GitHub PR Review

Use this when reviewing GitHub PRs and when posting review comments or code suggestions through `gh`.

This skill is specifically about publishing review feedback. If you are only reading code and reporting findings to the user, follow normal code-review practice and do not post anything to GitHub.

## Non-Negotiables

- Check `gh --version` before running GitHub review commands.
- Draft the complete review before posting.
- Show the user exactly what will be posted: file paths, line numbers, comment text, code suggestions, event type, and overall body.
- Get explicit approval before posting any public review comment.
- Use a pending review, even for one comment, then submit that pending review.

If `gh` is missing, stop and tell the user to install GitHub CLI and run `gh auth login`.

## Workflow

1. Verify tools and context:

```bash
gh --version
gh pr view <PR_NUMBER> --json commits --jq '.commits[-1].oid'
gh repo view --json owner,name
```

2. Review the PR and draft every comment locally.

3. Ask for approval. In Codex, ask the user directly unless a structured user-input tool is available. Include:

- Each comment's path and line.
- Exact body text, including any suggestion blocks.
- Review event: `COMMENT`, `APPROVE`, or `REQUEST_CHANGES`.
- Overall review body.

4. Create a pending review after approval:

```bash
gh api repos/:owner/:repo/pulls/<PR_NUMBER>/reviews \
  -X POST \
  -f commit_id="<COMMIT_SHA>" \
  -f 'comments[][path]=path/to/file.ts' \
  -F 'comments[][line]=42' \
  -f 'comments[][side]=RIGHT' \
  -f 'comments[][body]=Comment text' \
  --jq '{id, state}'
```

5. Submit the pending review:

```bash
gh api repos/:owner/:repo/pulls/<PR_NUMBER>/reviews/<REVIEW_ID>/events \
  -X POST \
  -f event="COMMENT" \
  -f body="Overall review message"
```

## Event Choice

- `REQUEST_CHANGES`: blocking correctness, security, data-loss, failing-test, or API-contract issues.
- `COMMENT`: neutral questions, observations, or feedback that should not block.
- `APPROVE`: approval with only non-blocking notes.

## Code Suggestions

Use GitHub suggestion blocks only when the replacement is complete and applies cleanly to the selected line range:

````markdown
```suggestion
replacement code
```
````

For markdown or documentation that already contains triple backticks, use four backticks or tildes around the suggestion block to avoid nesting conflicts.

## API Syntax Rules

- Quote `comments[][path]`, `comments[][line]`, `comments[][side]`, and `comments[][body]`.
- Use `-F` for numeric values such as `line` and `start_line`.
- Use `RIGHT` for added or modified lines and `LEFT` for deleted lines.
- Get the latest PR commit SHA before creating the review.

Stop if you are about to post directly, skip the approval step, or split feedback into separate review notifications for convenience.
