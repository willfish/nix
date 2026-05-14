---
name: systematic-debugging
description: >
  Rigorous root-cause debugging process. Use for ANY bug, test failure, unexpected behavior, performance issue, or when previous fix attempts have failed.
  Forces you to find the real cause before proposing or applying any fixes. This is the single most effective way to stop thrashing and actually solve hard problems.
metadata:
  short-description: "Systematic root cause debugging (no fixes before investigation)"
---

# Systematic Debugging (Grok Native)

**Iron Law:** NO FIXES WITHOUT ROOT CAUSE INVESTIGATION FIRST.

If you have not completed Phase 1, you are not allowed to propose or implement fixes. Violating this is the #1 reason debugging takes 3-10× longer than necessary.

## When to Use This Skill

Use for **every** technical issue:
- Test failures
- Production bugs
- Unexpected behavior
- Performance problems
- Build / CI failures
- "It worked yesterday" situations

Use it **especially** when:
- You're under time pressure
- A "quick fix" seems obvious
- You've already tried 1+ fixes that didn't work
- The issue feels mysterious

**Never skip it** just because the bug "looks simple".

## The Process (Four Phases)

You **must** complete each phase before moving to the next.

### Phase 1: Root Cause Investigation (Mandatory)

**Goal:** Understand *exactly* what is happening and *why*, before touching any code.

1. **Read every error message and stack trace completely**
   - Do not skim. Note file paths, line numbers, error codes.
   - Many bugs are solved by actually reading the error.

2. **Reproduce the issue reliably**
   - Can you make it happen on demand?
   - What are the exact preconditions?
   - If it's flaky, treat flakiness as part of the problem to investigate.

3. **Check what recently changed**
   - `git log --oneline -20`
   - `git diff HEAD~5` (or relevant range)
   - New dependencies, config, environment variables, infrastructure changes.

4. **Trace data and control flow (especially in multi-layer systems)**
   - In systems with multiple boundaries (workflow → build → signing, API → service → DB, etc.), add temporary diagnostic logging at each boundary.
   - Run once to see where the bad data/state first appears.
   - Fix the *source*, not the symptom you first noticed.

5. **Use supporting techniques when needed**
   - See the raw reference material in `~/.grok/skills-codex/superpowers/skills/systematic-debugging/` for advanced techniques (`root-cause-tracing.md`, `defense-in-depth.md`, `condition-based-waiting.md`).

**Success criteria for Phase 1:** You can explain, in one clear sentence, the root cause and why the symptoms appear.

Only after this can you move to Phase 2.

### Phase 2: Pattern Analysis

1. Find similar code that *works* in the same codebase.
2. Compare the working version against the broken one, line by line.
3. Identify every difference (no matter how small).
4. Understand all dependencies and implicit assumptions of the working code.

### Phase 3: Hypothesis & Minimal Testing

1. Form **one single, specific hypothesis**: "I believe the root cause is X because Y."
2. Write the hypothesis down.
3. Design the *smallest possible test* that would prove or disprove it (one variable at a time).
4. Run it.
   - If it confirms → move to Phase 4.
   - If not → form a new hypothesis and repeat. Do **not** stack more changes.

If you reach "I don't know" — say it out loud. Do not guess.

### Phase 4: Implementation & Verification

1. **Create a failing test first** (or minimal reproduction script). Use the `test-driven-development` skill if appropriate.
2. Make **one single change** that addresses the root cause you identified.
3. Verify:
   - The original failure is gone.
   - No other tests broke.
   - The fix is minimal and targeted.
4. If the fix doesn't work:
   - Stop.
   - Count how many fix attempts you've made.
   - < 3 attempts → return to Phase 1 with new information.
   - ≥ 3 attempts → **question the architecture** (see below). Do not attempt fix #4 without discussion.

**Architectural warning signs (after 3+ failed fixes):**
- Every fix reveals a new problem in a different place.
- Fixes require large refactors to implement cleanly.
- The same pattern of coupling or shared mutable state keeps appearing.

At this point the right answer is usually "this approach is fundamentally flawed" rather than "one more tweak."

## Red Flags — Immediate Return to Phase 1

If you catch yourself thinking or saying any of these, **stop**:

- "Quick fix for now, investigate later"
- "Let me just try changing X and see"
- "I'll make a few changes and run the tests"
- "Skip the test, I'll verify manually"
- "It's probably X, I'll fix that"
- "I don't fully understand it but this should work"
- "One more fix attempt" (when you've already done 2+)
- Proposing solutions before you've traced the data flow

These thoughts are the enemy. They are how you waste hours and introduce new bugs.

## Use With Other Skills

This skill works extremely well together with:
- `superpowers` (the overall discipline)
- `verification-before-completion` (after you think you've fixed it)
- `test-driven-development` (for Phase 4)
- `writing-plans` (if the debugging session is large or architectural)

## Why This Matters

Raw intelligence + good intentions is not enough. The difference between a 15-30 minute fix and 2-3 hours of thrashing is almost always whether you followed a systematic process.

Use this skill. It will save you (and your future self) enormous amounts of time and pain.

---

**Supporting reference material** lives in:
`~/.grok/skills/references/superpowers/skills/systematic-debugging/`

It contains the deeper original documents (`root-cause-tracing.md`, `defense-in-depth.md`, `condition-based-waiting.md`, test pressure examples, etc.). Read them when you need more detail than this skill provides.
