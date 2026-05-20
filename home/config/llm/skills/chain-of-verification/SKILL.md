---
name: chain-of-verification
description: Use for architecture decisions, research, strategic analysis, risk assessment, RFCs, specs, or any answer where being wrong would waste significant time or mislead downstream work.
---

# Chain Of Verification

Use Chain of Verification for high-stakes reasoning work:

1. Baseline: produce the initial analysis, assumptions, recommendation, or plan.
2. Challenge: independently attack the baseline. Look for wrong assumptions, missed alternatives, overconfidence, missing evidence, and contradictory data.
3. Synthesis: merge both passes. Keep what survived challenge, incorporate missed points, and flag residual uncertainty.

With subagents, only use them if the user has explicitly allowed subagent delegation. Give independent challengers the same raw inputs first; do not leak your baseline until they have formed their own view.

Read `references/planning.md` for the full workflow and output structure.
