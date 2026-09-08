---
description: Review changes for concrete bugs and missing verification
argument-hint: "[files, commit or comparison base]"
---
Review the following scope, or the current staged and unstaged tracked changes
if no scope is supplied:
$ARGUMENTS

Inspect the working tree, diff, surrounding implementation and relevant tests.
Respect repository instructions and distinguish pre-existing changes from the
work under review. Trace the behavior before reporting a finding.

Prioritize concrete correctness bugs, regressions, unsafe data handling and
missing verification that could conceal a defect. For each finding give the
file and line, triggering conditions, user impact and a suggested correction.
Avoid speculative problems and style-only suggestions. State clearly when
you find no actionable issues and identify any remaining verification gaps.

Run appropriate existing checks through the repository environment when
useful. Do not edit source files, install dependencies, commit, push or publish
a review. Return the findings to me, with the most consequential issue first.
