---
name: verification-before-completion
description: >
  Completion verification gate. Use before saying done, fixed, passing, it works,
  ready, or before commits, pushes, PRs, and completion claims.
metadata:
  short-description: "Never claim success without fresh verification evidence"
---

# Verification Before Completion

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

**Iron Law:** NO COMPLETION CLAIMS WITHOUT FRESH VERIFICATION EVIDENCE.

Verify the current artifact in its actual environment before claiming completion.
Fresh evidence is tied to that artifact, not to how many status messages or tool
calls have occurred.

## Choose proportionate verification

For small utilities and direct ports, default to one representative end-to-end
manual check after implementation, plus existing required checks. Use safe offline
fixtures where live execution needs separate authorization. Do not manufacture a
new test suite, parity harness, failure-injection rig or sanitizer matrix merely
to satisfy this skill. Add targeted tests for observed defects, explicit
requirements or material safety risks.

Reuse evidence within an unchanged batch. Repeat affected checks after relevant
code, configuration, environment or extraction changes, not merely after a commit
or progress message. Preserve authorization gates and report coverage honestly.

## The Gate

Before you say anything like:
- "It's fixed"
- "Tests pass"
- "Build succeeds"
- "The bug is resolved"
- "Requirements are met"
- "This is ready for review"

The evidence for the current artifact must establish all of the following:

1. **Identify** the exact command(s) that would prove the claim.
2. **Run** the required checks in the actual worktree/environment, once the implementation is stable.
3. **Read** the complete output, including exit codes and any failure counts.
4. **Confirm** that the output actually supports the claim you want to make.
5. Only *then* make the claim and include key evidence in your response to the requester.
6. If this session changed `~/.dotfiles`, do not claim done until that change is committed on master, `hmswitch` has succeeded, and the commit is pushed. Another repository as cwd does not waive this. Skip only when the user explicitly said not to commit, switch, or push.

Keep full output in the execution record, not the published artifact. This gate
requires running checks, not adding verification prose to PRs, commits, or
architecture docs. Keep all verification details out of PR bodies, including
non-CI evidence. Report results, coverage gaps and blockers in the conversation;
follow `~/.agents/guides/documentation-relevance.md` for placement.

Before creating a PR, check the risk-label workflow's accepted body format and
include its required risk marker at creation time; a label applied afterwards
may arrive too late. If auto-merge still sees an earlier failed risk-label run
after newer runs pass, rerun the failed run with user authorization and verify
its result. A newer successful run may not clear the historical failure.

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

This skill is designed to be used **after** `systematic-debugging`, `writing-plans`, or any implementation work.

Recommended pairing:
- Do the work using the appropriate skill(s)
- When you think you're done → invoke `verification-before-completion`
- Only after passing this gate do you summarize for the user

## Red Flags (You Are About to Cheat)

If you feel the urge to:
- Say "it should be good now"
- Rely on checks for an older artifact or a different environment
- Skip required checks or claim more coverage than the checks establish
- Trust that your change "obviously" fixed it
- Skip verification because "the user is waiting"

→ **Stop.** These are the moments where this skill exists to protect you from yourself.

## Practical Tips

- Use `run_command` with the real command the project uses (e.g. `nix flake check`, `cargo test`, `npm test`, `home-manager build`, etc.).
- For complex verification, use `todo_write` to track the verification steps themselves.
- When the verification fails, treat it as new information and usually return to `systematic-debugging`.
- For very important work, combine with `best-of-n` or a fresh subagent review.

---

**Philosophy:** Evidence before assertions. Always.

This single habit, applied consistently, dramatically improves the quality and reliability of the work you produce.

**Supporting material** (original detailed version) is available in `~/.agents/references/superpowers/skills/verification-before-completion/`.
