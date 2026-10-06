---
name: writing-plans
description: >
  Implementation planning. Use for specs, feature requests, multi-step changes,
  complex tasks, handoff plans, file-level tasks, and expected
  verification commands before coding.
metadata:
  short-description: "Write excellent, actionable implementation plans (small steps, clear handoff)"
---

# Writing Plans

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

**Goal:** Make the scope, approach and finish line clear without planning the implementation twice.

For small utilities and straightforward ports, use a short checklist and implement
directly. Do not write a formal plan, prewrite the code, or invent a test matrix.
Plan one representative end-to-end manual check plus existing required checks.
Expand the plan only for genuine architectural uncertainty, dependencies or
material risk. The detailed format below is for that larger work.

## Core Principles

- Assume the implementer is competent but has **zero** context on this specific problem or codebase.
- Group related changes into coherent, bounded deliveries.
- Specify important interfaces, constraints and check commands; do not duplicate the implementation in prose.
- Do not use TDD. Implement first, then verify; add tests where the task or evidence calls for them.
- Use one final verification and release cycle per bounded batch where practical.
- The plan itself should be reviewable and executable.

## Recommended Workflow

1. If the work is large or architectural, first use `enter_plan_mode` to explore and think.
2. Use this skill to produce the actual plan document.
3. (Optional but recommended) Use `todo_write` while creating the plan to track your own decomposition.
4. Save the plan to the project's preferred local gitignored planning
   location. In this dotfiles harness, default to
   `plans/YYYY-MM-DD-<short-name>.md`. Do not add or commit plan/spec files.
5. If a human must choose how to execute, call the question tool with concrete
   options such as Subagent-driven, Inline, and Do not implement. Do not ask
   that choice in the chat editor.

## Detailed plan structure

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
### Task 3: Implement thing behavior

**Files:**
- Modify: `src/feature/thing.ts`
- Modify: `tests/feature/thing.test.ts`

- [ ] Implement the behavior
  ```ts
  export function doThing(input: Input): Result {
    return compute(input);
  }
  ```

- [ ] Add or update tests for the evolved implementation
  ```ts
  it('should do X when Y', () => {
    const result = doThing(input);
    expect(result).toBe(expected);
  });
  ```

- [ ] Run the tests and confirm they pass
  ```bash
  pnpm test tests/feature/thing.test.ts
  ```
  Expected: PASS

- [ ] Commit
  ```bash
  git add src/feature/thing.ts tests/feature/thing.test.ts
  git commit -m "feat: implement thing behavior"
  ```
```

## Rules for High-Quality Plans

- **No placeholders.** Never write "TBD", "implement proper error handling", "add tests later", or "similar to Task 5".
- Include concrete commands and decisive interface details, not a second copy of the implementation.
- Each delivery should leave the codebase in a working, testable state.
- Prefer a few coherent tasks over unnecessary fragmentation.
- Include verification commands with expected output where possible.
- When modifying existing code, show the relevant before/after context or exact edit location.

## Self-Review Before Handing Off

After writing the full plan, run this checklist:

1. Does every requirement from the original spec have at least one task that clearly implements it?
2. Are there any "magic" steps that assume the implementer already knows something important?
3. Is the plan specific enough to act on without prewriting the implementation?
4. Have I added complexity, edge cases or checkpoints unsupported by the task?

Fix anything you find.

## Execution Handoff

Once the plan is written and saved, ask the user:

> Plan saved to `plans/...`.
> Do you want to:
> 1. Execute it now (I'll drive using the plan as checklist), or
> 2. Use subagents (recommended for larger plans — one fresh subagent per task with review between)?

For subagent execution, the `superpowers` skill + `spawn_subagent` works very well.

---

The full original material (with additional examples) lives in:
`~/.agents/references/superpowers/skills/writing-plans/`
Ignore TDD in that upstream copy.

This skill pairs extremely well with `enter_plan_mode`, `superpowers`, and `systematic-debugging`.
