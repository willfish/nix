# Planning & Verification

## When to plan

Before implementing anything that involves:
- Multiple files or services
- Architectural decisions
- Research or analysis
- Non-trivial refactoring
- Anything where being wrong wastes more than 30 minutes

## Inline planning (quick tasks)

For straightforward multi-step work:

```
PLAN:
1. [step] — [why]
2. [step] — [why]
3. [step] — [why]
→ Executing unless you redirect.
```

## Chain of Verification (strategic/research/analysis tasks)

For anything where accuracy matters — research, architecture decisions, evaluating approaches, writing documentation that others will rely on — use Chain of Verification (CoV).

CoV catches errors that single-pass reasoning misses. In testing, it improved accuracy from ~68% to ~94%.

### The process

**Step 1: Baseline**
Generate your initial analysis/plan/recommendation. Be thorough. State your assumptions. This is your first draft.

**Step 2: Challenge (independent)**
Now forget your baseline. Look at the same problem fresh. Actively try to find:
- Wrong assumptions in the baseline
- Things the baseline missed entirely
- Places where the baseline is overconfident
- Alternative approaches not considered
- Evidence that contradicts the baseline

The challenge must be adversarial. Don't just agree with yourself.

**Step 3: Synthesis**
Merge baseline + challenge into a final output:
- Where they agree → high confidence
- Where they disagree → investigate, pick the stronger argument, flag uncertainty
- What the challenge found that the baseline missed → incorporate
- What the baseline got right that the challenge couldn't break → keep

### When to use CoV

| Task | Use CoV? |
|------|----------|
| Architecture decision | Yes |
| Evaluating competing approaches | Yes |
| Research/competitive analysis | Yes |
| Writing specs or RFCs | Yes |
| Risk assessment | Yes |
| Routine bug fix | No |
| Simple feature implementation | No |
| Style/formatting changes | No |

### With subagents

When using subagents for planning:
1. First subagent produces the baseline analysis
2. Second subagent independently challenges it (do NOT show it the baseline first — give it the same raw inputs and let it form its own view, then show the baseline and ask it to challenge)
3. You synthesise the final result

This mirrors how code review works — the reviewer hasn't seen your thought process, only the output. Fresh eyes catch what familiar eyes miss.

### Output format

```markdown
## Baseline
[Initial analysis]

## Challenge
[Independent critique — what's wrong, what's missing]

## Synthesis
[Final verified output with confidence levels]

### Confidence: HIGH / MEDIUM / LOW
[One-line justification]
```

## Planning anti-patterns

- **Planning without verifying** — a plan is just a guess until challenged
- **Challenging without independence** — if the challenger has seen the baseline first, it anchors to it
- **Skipping synthesis** — two opinions without a verdict is worse than one opinion
- **Over-planning simple tasks** — CoV for a CSS change is waste
- **Under-planning complex tasks** — "I'll figure it out as I go" on a multi-service change is how you get half-finished refactors
