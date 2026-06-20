# Harness Discipline

These rules are always-on guardrails for agent sessions. They do not replace
skills; they stop core workflow discipline from depending on voluntary recall.

## Skills First

- Before non-trivial work, check whether a relevant skill applies.
- If a skill may apply, load its `SKILL.md` or invoke it before exploring,
  clarifying, planning, or editing.
- If unsure which skill applies, use `skill-router`.
- In Grok, descriptions are not enough: run `/skills <name>` or read the skill
  file directly.

Red flags that mean a skill check is being skipped:

- "This is just a simple question."
- "I need more context first."
- "Let me explore the code first."
- "I can do this quickly without a skill."
- "This feels productive, I'll just start."

## Track Work

- Use the todo/checklist tool for work with 3 or more meaningful steps, or work
  that will take more than a few minutes.
- Keep statuses current as the work changes.
- Break tasks into verifiable chunks.

## Plan When Risky

- Use plan mode and a written plan for architecture, multi-file refactors,
  research-heavy work, high-risk changes, or anything where being wrong wastes
  significant time.
- For new behavior or feature work, prefer brainstorming/design first, then an
  implementation plan.

## Verify Before Completion

- Do not claim done, fixed, passing, ready, pushed, or reviewed without fresh
  verification from this worktree.
- Identify the real command that proves the claim, run it, read the output and
  exit status, then report the evidence.
- For PRs, commits, extracted refactors, object-construction changes, and test
  fixes, verification is mandatory.

## Keep Scope Clean

- Do not revert user changes unless explicitly asked.
- Prefer small focused edits that match the repository's existing patterns.
- Use Nix for ephemeral tooling; do not mutate the host or project manifests for
  temporary agent needs.
