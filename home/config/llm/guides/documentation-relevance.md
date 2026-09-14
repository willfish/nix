# Write for the reader, not the session

**Published prose is not an agent's work diary.** Include a detail only when it
helps the intended reader understand, decide, act, or maintain. Being true,
recent, or expensive to discover does not make it worth publishing.

## The relevance gate

Before writing, identify the reader, the artifact's purpose, and its useful
lifetime. For each detail ask: **What would the reader misunderstand or do
wrong without this?** If there is no concrete answer, delete it.

- Keep intent, behaviour, contracts, constraints, significant decisions,
  trade-offs, consequences, and material uncertainty.
- Drop exploration chronology, failed tool invocations, local environment
  detours, agent coordination, scratch paths, temporary counts, and claims of
  effort or diligence. Turn a useful discovery into a rule, not an anecdote.
- Do not repeat what the diff, CI, ticket fields, or linked source already says.
  Keep enough context to make the explanation intelligible without link chasing.
- Use exact names, dates, versions, counts, and commands only when they affect
  understanding or action. Precision is not a reason to include irrelevant facts.
- Do not add headings, diagrams, summaries, praise, or caveats just to fill a
  template. Honour required fields with the minimum useful content.
- Edit the final artifact against the final change. In current-state prose,
  remove superseded plans and resolved blockers; do not append a correction diary.

## Put detail where it belongs

| Artifact | Keep | Leave out |
|---|---|---|
| Architecture, ADRs, reference docs | System boundaries, invariants, decision context and consequences; significant rejected alternatives and why | Implementation diary, current PR status, test runs, workstation details |
| PR body | Problem, meaningful behaviour change, rationale, reviewer-relevant risk and rollout requirements | All verification details, including commands, results, CI status, manual evidence and coverage reports; implementation diaries |
| Commit | Change and why; non-obvious consequences; Jira footer when a ticket exists | Test counts, build status, branch operations, agent process |
| Code comment | Non-obvious reason, invariant, constraint, or necessary algorithm explanation | Restating code, announcing an edit, author/session history |
| Review or issue comment | Actionable finding, supporting evidence, impact, requested decision | Investigation transcript, generic praise, repeated status |
| Runbook or how-to | Reproducible prerequisites, commands, expected signals, failure handling | One operator's completed run or machine-specific workaround presented as universal |
| Status update, handoff, local plan | Outcomes, active blockers, next action; execution detail needed by the recipient | Routine activity or resolved detours that do not affect the handoff |
| Skill or guide | Reusable instructions, decision rules, necessary examples | The incident or session that taught the rule, unless needed to explain it |

**Verification is work you must do, not content for PR bodies.** Run the
required checks and inspect their results. Never include test commands, results,
counts, CI status, build logs, manual checks, screenshots, benchmarks or
verification narratives in a PR body. This includes non-CI evidence: do not move
it into What, Why or Risk to avoid a verification heading. Omit template
verification fields unless the user explicitly requests them.

Report verification evidence, coverage gaps and blockers to the requester in the
conversation. Keep full execution logs in the session or an appropriate evidence
store. Preserve material risks and rollout requirements in PR bodies, without
turning them into reports of checks performed. Do not copy completion evidence
into commits or architectural docs.

**Transient does not mean disposable everywhere.** Incident timelines, dated
status reports, audit/tax records, reproducible bug reports, and temporary rollout
constraints can require exact historical facts. Preserve them in their proper
record. Do not erase accepted ADR history, required evidence, failures, or
material uncertainty in the name of brevity. A current architecture overview and
a historical decision record have different jobs.

## Before and after

- Architecture: “After three failed runs on my laptop we added deduplication;
  84 tests pass.” → “Deduplicate by event ID because delivery is at least once.”
- PR: “Updated four files, reran the suite, 5310 examples passed, CI green.”
  → Delete. Explain the behaviour change elsewhere; CI owns its result.
- PR manual-check narrative: Move it to the conversation, even when CI does not
  cover the behaviour. Keep only the intended behaviour and material risks in
  the PR body.
- Commit: “Finally fixed retries after debugging and rerunning CI.”
  → “Bound retries to prevent duplicate charges during gateway outages.”
- Comment: “Changed this to 30 after yesterday's failure.”
  → “Keep the lease longer than the upstream request timeout.”

Final pass: **Could this have been written by someone who knows the finished
system but never saw the agent session?** If not, justify the session detail
against the artifact's purpose or remove it. Shorter is not automatically better;
less work for the reader is.

## Research behind the rules

These are editorial rules synthesised from the sources below, not a claim that
research establishes an optimal length or a universal ban on historical detail.

- [BetterUp Labs / Stanford Social Media Lab: Workslop](https://www.betterup.com/workslop):
  polished AI output can transfer interpretation and cleanup to colleagues.
  Self-reported workplace survey, not a documentation-specific causal study.
- [Google: Writing good CL descriptions](https://google.github.io/eng-practices/review/developer/cl-descriptions.html):
  describe what and why for reviewers and future readers; review the final description.
- [Chris Beams: How to Write a Git Commit Message](https://cbea.ms/git-commit/):
  preserve motivation and context rather than narrating implementation mechanics.
- [AWS: Architectural decision record process](https://docs.aws.amazon.com/prescriptive-guidance/latest/architectural-decision-records/adr-process.html):
  record significant decisions, context, and consequences; preserve decision history.
- [Diátaxis: Explanation](https://diataxis.fr/explanation/):
  retain explanatory context and historical reasons while bounding the topic.
- [Google: What to look for in a code review](https://google.github.io/eng-practices/review/reviewer/looking-for.html):
  comments should usually explain reasons the code cannot convey.
- [Simon Willison: Your job is to deliver code you have proven to work](https://simonwillison.net/2025/Dec/18/code-proven-to-work/):
  verification must not be offloaded to reviewers. Keeping verification out of
  PR bodies is our editorial policy, not his prescription to omit evidence.
