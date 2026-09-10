---
name: chain-of-verification
description: Architecture, research, and risk verification. Use for RFCs, specs, strategic analysis, architecture decisions, or high-cost-to-be-wrong answers.
---

# Chain Of Verification

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use Chain of Verification for high-stakes reasoning work:

1. Baseline: produce the initial analysis, assumptions, recommendation, or plan.
2. Challenge: independently attack the baseline. Look for wrong assumptions, missed alternatives, overconfidence, missing evidence, and contradictory data.
3. Synthesis: merge both passes. Keep what survived challenge, incorporate missed points, and flag residual uncertainty.

With subagents, only use them if the user has explicitly allowed subagent delegation. Give independent challengers the same raw inputs first; do not leak your baseline until they have formed their own view.

Read `references/planning.md` for the full workflow and output structure.
