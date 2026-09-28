# Necessary and Sufficient Tests

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Source: https://testdouble.com/insights/necessary-sufficient
Checked: 2026-09-28
Update trigger: recurring over-specified or under-asserted tests, or a material change to this heuristic.

Use this when writing or reviewing tests. It is a design check, not a TDD mandate. Implement the behaviour first, then hold the tests to this rule.

Justin Searls records Jim Weirich's standard: every test should be both necessary and sufficient. The point is a small, objective correction instead of a fresh argument about thoroughness for each example.

## Rule

- Necessary: test only the situations required to exercise the code's real behaviour.
- Sufficient: assert every behaviour a replacement implementation would need in order to be considered working and complete.

Both must hold. A short test can still miss behaviour. A large matrix can still specify distinctions the code does not make.

## Necessary

An unnecessary test describes differences that do not change the outcome.

- If two inputs take the same path and produce the same observable result, one example is enough.
- Do not add combinations for flags, arguments, or future edge cases the code ignores. Extra cases imply interactions that are not there.
- Coverage cannot justify a redundant example. Once each distinct path is exercised, more examples do not make the suite more complete.
- Add a case when the behaviour exists, not because it might matter later.
- Question generated cases, record-playback snapshots, and mocks that fail on any unexpected call when they specify more than the behaviour under test.

Judge this by redundant coverage and by whether removing the example would leave a real path unexercised.

## Sufficient

An insufficient test leaves important behaviour unasserted. Running a line is not the same as specifying what that line does. Coverage is a one-way signal: it can show where tests are absent, not that the tests which are present are enough.

- Assert the outcomes a replacement would have to preserve: return values, persisted state, collaborator calls and their arguments, and the absence of a side effect when that absence is part of the contract.
- Practical check: deleting a meaningful line or branch should fail a test. If the suite still passes, that behaviour is not specified.
- Do not stop at a happy return value when the code also changes records, collaborators, or shared state.
- If a behaviour is hard to assert, treat that as a design signal, not a reason to skip it. Hard-to-test behaviour is often hard-to-use behaviour: a dishonest name, a command that also queries, hidden global state, or mixed levels of abstraction. Fix that design when it is in scope. Do not replace the missing assertion with a weaker test.
- Assert an interaction only when it is part of the behaviour. Do not add a double merely to silence an irrelevant collaborator.

## How to apply

1. List the distinct paths and observable effects of the code as it is.
2. Write one example per path that changes an outcome. Include invalid and edge paths only when they are real branches or contracts.
3. Assert each effect a replacement would have to preserve.
4. Remove an example that still leaves its path exercised, and add an assertion where deleting a meaningful line still passes.
5. Keep the local rule: no TDD. Use the heuristic on tests added after the behaviour exists, including a regression test for a bug.

## Out of scope

- Do not test framework behaviour, private methods, or trivial accessors. Those are not behaviour this suite owns.
- Do not expand into combinatorial cases, snapshots, or a double for every collaborator in order to look complete.
- Load the RSpec structure, factory, Rails, and flakiness references for mechanics. This guide does not replace them.
