---
name: writing-plans
description: >
  Create high-quality, detailed implementation plans for multi-step work before writing code.
  Use when you have a spec, feature request, or complex task. Produces bite-sized, verifiable tasks with exact file paths, code examples, commands, and expected outcomes. Strongly encourages TDD and frequent commits.
metadata:
  short-description: "Write excellent, actionable implementation plans (TDD, small steps, clear handoff)"
---

# Writing Plans (Grok Native)

**Goal:** Produce a plan so clear and detailed that a skilled developer (or subagent) who has never seen the codebase can implement it correctly with minimal additional context.

**When to use:** Any non-trivial feature, refactor, or bug fix that will take more than a few focused steps. Especially valuable before entering heavy implementation.

## Core Principles

- Assume the implementer is competent but has **zero** context on this specific problem or codebase.
- Every task must be small enough to complete in 2–15 minutes.
- Every code-changing step must include the actual code (no "implement X" placeholders).
- Prefer TDD: failing test → minimal implementation → passing test → commit.
- Frequent small commits are better than large ones.
- The plan itself should be reviewable and executable.

## Recommended Workflow

1. If the work is large or architectural, first use `enter_plan_mode` to explore and think.
2. Use this skill to produce the actual plan document.
3. (Optional but recommended) Use `todo_write` while creating the plan to track your own decomposition.
4. Save the plan to `docs/plans/YYYY-MM-DD-<short-name>.md` (or your project's preferred location).
5. Offer the user two execution modes:
   - **Subagent-driven** (recommended for complex work): One fresh subagent per task with review between tasks.
   - **Inline**: Execute the plan yourself in this session using checkpoints.

## Plan Structure (Required)

Every plan should start with this header:

```markdown
# [Feature / Fix Name] Implementation Plan

**Goal:** One clear sentence describing the outcome.

**Approach:** 2-4 sentences on the architecture/strategy.

**Key Files:**
- `path/to/new-file.ts` (new)
- `path/to/existing.ts:45-120` (modify)
- `tests/path/to/test.ts` (new or modify)

**Tech Notes:** Relevant patterns, libraries, constraints.

---
```

Then break the work into tasks using this format:

```markdown
### Task 3: Add failing test for new behavior

**Files:**
- Create: `tests/feature/thing.test.ts`
- (No other files in this task)

- [ ] Write the minimal failing test that describes the desired behavior
  ```ts
  it('should do X when Y', () => {
    const result = doThing(input);
    expect(result).toBe(expected);
  });
  ```

- [ ] Run the test and confirm it fails for the right reason
  ```bash
  pnpm test tests/feature/thing.test.ts
  ```
  Expected: FAIL with message about missing implementation

- [ ] Commit
  ```bash
  git add tests/feature/thing.test.ts
  git commit -m "test: add failing test for thing behavior"
  ```
```

## Rules for High-Quality Plans

- **No placeholders.** Never write "TBD", "implement proper error handling", "add tests later", or "similar to Task 5".
- Every code step must contain real, copy-pasteable code or commands.
- Each task should leave the codebase in a working, testable state.
- Prefer many small tasks over fewer large ones.
- Include verification commands with expected output where possible.
- When modifying existing code, show the relevant before/after context or exact edit location.

## Self-Review Before Handing Off

After writing the full plan, run this checklist:

1. Does every requirement from the original spec have at least one task that clearly implements it?
2. Are there any "magic" steps that assume the implementer already knows something important?
3. Did I include actual code/commands instead of descriptions?
4. Are the tasks small enough that a subagent could do one per invocation?

Fix anything you find.

## Execution Handoff

Once the plan is written and saved, ask the user:

> Plan saved to `docs/plans/...`.
> Do you want to:
> 1. Execute it now (I'll drive using the plan as checklist), or
> 2. Use subagents (recommended for larger plans — one fresh subagent per task with review between)?

For subagent execution, the `superpowers` skill + `spawn_subagent` works very well.

---

The full original material (with additional examples) lives in:
`~/.grok/skills/references/superpowers/skills/writing-plans/`

This skill pairs extremely well with `enter_plan_mode`, `superpowers`, and `systematic-debugging`.
