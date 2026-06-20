# Harness Discipline

These rules are always-on guardrails for agent sessions. They do not inject
skill bodies automatically. They carry the minimum discipline that must always
be in context, then point to skills for deeper task-specific procedure.

## Skills First

- Before non-trivial work, check whether a relevant skill applies.
- If a skill may apply, load its `SKILL.md` or invoke it before exploring,
  clarifying, planning, or editing.
- If unsure which skill applies, use `skill-router`.
- In Grok, descriptions are not enough: run `/skills <name>` or read the skill
  file directly.

Minimum always-on algorithm:

1. Classify the task: simple answer, bug/debugging, implementation, plan/RFC,
   review, PR/Jira, tests, dotfiles/Nix, docs, or design.
2. Match the task to the skill list or routing table.
3. Load the matched skill before the first substantive action.
4. If no skill fits, say that briefly and proceed with normal engineering
   judgment.
5. For work that continues past a quick answer, create and maintain a checklist.

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
