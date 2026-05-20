---
name: verification-before-completion
description: Use when about to claim work is complete, fixed, or passing — especially before committing, pushing, creating PRs, or extracting refactors. Requires running verification commands with fresh output evidence before any success claim. Extremely strict on value object refactors (Struct → Data.define, etc.) and PR extraction work. Evidence before assertions, always.
---

# Verification Before Completion

## Overview

Claiming work is complete without verification is dishonesty, not efficiency.

**Core principle:** Evidence before claims, always.

**Violating the letter of this rule is violating the spirit of this rule.**

## The Iron Law

```
NO COMPLETION CLAIMS WITHOUT FRESH VERIFICATION EVIDENCE
```

If you haven't run the verification command in this message, you cannot claim it passes.

## The Gate Function

```
BEFORE claiming any status or expressing satisfaction:

1. IDENTIFY: What command proves this claim?
2. RUN: Execute the FULL command (fresh, complete)
3. READ: Full output, check exit code, count failures
4. VERIFY: Does output confirm the claim?
   - If NO: State actual status with evidence
   - If YES: State claim WITH evidence
5. ONLY THEN: Make the claim

Skip any step = lying, not verifying
```

## Common Failures

| Claim | Requires | Not Sufficient |
|-------|----------|----------------|
| Tests pass | Test command output: 0 failures | Previous run, "should pass" |
| Linter clean | Linter output: 0 errors | Partial check, extrapolation |
| Build succeeds | Build command: exit 0 | Linter passing, logs look good |
| Bug fixed | Test original symptom: passes | Code changed, assumed fixed |
| Regression test works | Red-green cycle verified | Test passes once |
| Agent completed | VCS diff shows changes | Agent reports "success" |
| Requirements met | Line-by-line checklist | Tests passing |

## High-Risk Refactors (Extra Strict Mode)

These changes have historically caused "it worked in the original branch but broke after extraction" failures:

- Switching from `Struct` / `OpenStruct` to `Data.define` (or vice versa)
- Changing value object construction (adding required fields, removing mutability)
- Refactors that affect how internal Result/Context objects are instantiated
- Any change that makes previously optional/omittable arguments required

**For these changes you must:**
1. Identify every class whose construction semantics are changing.
2. Search the **entire** `spec/` directory for every `::Result.new(`, `::Context.new(`, `::new(` call on those classes.
3. Run the full relevant test groups from the worktree (`rspec spec/services/ spec/requests/` for the affected domains) **before** pushing or creating the PR.
4. Treat "the original mixed commit was green" as irrelevant — extraction can expose previously hidden partial construction in tests.

Never rely on "I grepped and it looked fine." Run the tests.

## Red Flags - STOP

- Using "should", "probably", "seems to"
- Expressing satisfaction before verification ("Great!", "Perfect!", "Done!", etc.)
- About to commit/push/PR without verification
- Trusting agent success reports
- Relying on partial verification
- Thinking "just this once"
- Tired and wanting work over
- **ANY wording implying success without having run verification**

## Rationalization Prevention

| Excuse | Reality |
|--------|---------|
| "Should work now" | RUN the verification |
| "I'm confident" | Confidence ≠ evidence |
| "Just this once" | No exceptions |
| "Linter passed" | Linter ≠ compiler |
| "Agent said success" | Verify independently |
| "I'm tired" | Exhaustion ≠ excuse |
| "Partial check is enough" | Partial proves nothing |
| "Different words so rule doesn't apply" | Spirit over letter |

## Key Patterns

**Tests:**
```
✅ [Run test command] [See: 34/34 pass] "All tests pass"
❌ "Should pass now" / "Looks correct"
```

**Regression tests (TDD Red-Green):**
```
✅ Write → Run (pass) → Revert fix → Run (MUST FAIL) → Restore → Run (pass)
❌ "I've written a regression test" (without red-green verification)
```

**Build:**
```
✅ [Run build] [See: exit 0] "Build passes"
❌ "Linter passed" (linter doesn't check compilation)
```

**Requirements:**
```
✅ Re-read plan → Create checklist → Verify each → Report gaps or completion
❌ "Tests pass, phase complete"
```

**Agent delegation:**
```
✅ Agent reports success → Check VCS diff → Verify changes → Report actual state
❌ Trust agent report
```

## Why This Matters

From 24 failure memories:
- your human partner said "I don't believe you" - trust broken
- Undefined functions shipped - would crash
- Missing requirements shipped - incomplete features
- Time wasted on false completion → redirect → rework
- Violates: "Honesty is a core value. If you lie, you'll be replaced."

**Real example (2026-05):**
A `Struct` → `Data.define` refactor was extracted into its own PR. All production call sites were updated correctly, but several test stubs constructing `InteractiveSearchService::Result` (and similar) omitted the new required `result_limit` field. The original mixed commit was green, so targeted greps felt "good enough." CI failed after PR creation. The fix required a second commit on the refactor PR. This exact failure mode is now called out in the High-Risk Refactors section.

## When To Apply

**ALWAYS before:**
- ANY variation of success/completion claims
- ANY expression of satisfaction
- ANY positive statement about work state
- Committing, pushing, or creating a PR
- Extracting changes into a separate branch/PR
- Moving to next task
- Delegating to agents

**Especially strict before:**
- Any refactor involving `Struct` → `Data.define`, `OpenStruct` removal, or value object changes
- PR extraction work (pulling one type of change out of a larger branch)
- Any situation where you are tempted to say "the original commit was green" or "I already grepped"

**Rule applies to:**
- Exact phrases
- Paraphrases and synonyms
- Implications of success
- ANY communication suggesting completion/correctness

## The Bottom Line

**No shortcuts for verification.**

Run the command. Read the output. THEN claim the result.

This is non-negotiable.
