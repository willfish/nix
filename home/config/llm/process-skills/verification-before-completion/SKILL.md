---
name: verification-before-completion
description: >
  Mandatory verification gate before claiming any work is complete, fixed, or passing.
  Use when you are about to say "done", "fixed", "tests pass", "it works", or similar — requires fresh evidence from running the actual verification command(s) in the current context.
metadata:
  short-description: "Never claim success without fresh verification evidence"
---

# Verification Before Completion (Grok Native)

**Iron Law:** NO COMPLETION CLAIMS WITHOUT FRESH VERIFICATION EVIDENCE.

Claiming something is done without actually verifying it in this turn is not efficiency — it is lying to the user and to yourself.

## The Gate (Run This Every Time)

Before you say anything like:
- "It's fixed"
- "Tests pass"
- "Build succeeds"
- "The bug is resolved"
- "Requirements are met"
- "This is ready for review"

You **must** do all of the following in the current response:

1. **Identify** the exact command(s) that would prove the claim.
2. **Run** the full, fresh command(s) using the `run_command` tool (or `todo_write` + commands if multi-step).
3. **Read** the complete output, including exit codes and any failure counts.
4. **Confirm** that the output actually supports the claim you want to make.
5. Only *then* make the claim — and include the key evidence in your summary.

Skipping any of these steps = invalid claim.

## Common Claims and What "Verification" Actually Means

| Claim you want to make     | What you must actually run & show                  | Not acceptable |
|----------------------------|----------------------------------------------------|----------------|
| "Tests pass"               | Full test command output showing 0 failures        | "I ran them earlier", "they should pass" |
| "Linter is clean"          | Full linter output with 0 errors/warnings          | Partial directory, previous run |
| "Build succeeds"           | Clean build command exit code 0 + relevant logs    | "It compiled locally last week" |
| "Bug is fixed"             | Reproduce original failing case → now passes       | Code changed + assumption |
| "No regressions"           | Relevant test suite after the change               | "Only the new test passes" |
| "Requirements met"         | Line-by-line checklist against original request    | High-level summary only |

## How to Use This With Other Skills

This skill is designed to be used **after** `systematic-debugging`, `writing-plans`, `test-driven-development`, or any implementation work.

Recommended pairing:
- Do the work using the appropriate skill(s)
- When you think you're done → invoke `verification-before-completion`
- Only after passing this gate do you summarize for the user

## Red Flags (You Are About to Cheat)

If you feel the urge to:
- Say "it should be good now"
- Reference a previous run of the command
- Run a subset of the verification ("just the important tests")
- Trust that your change "obviously" fixed it
- Skip verification because "the user is waiting"

→ **Stop.** These are the moments where this skill exists to protect you from yourself.

## Practical Tips for Grok

- Use `run_command` with the real command the project uses (e.g. `nix flake check`, `cargo test`, `npm test`, `home-manager build`, etc.).
- For complex verification, use `todo_write` to track the verification steps themselves.
- When the verification fails, treat it as new information and usually return to `systematic-debugging`.
- For very important work, combine with `best-of-n` or a fresh subagent review.

---

**Philosophy:** Evidence before assertions. Always.

This single habit, applied consistently, dramatically improves the quality and reliability of the work you produce.

**Supporting material** (original detailed version) is available in `~/.grok/skills/references/superpowers/skills/verification-before-completion/`.
