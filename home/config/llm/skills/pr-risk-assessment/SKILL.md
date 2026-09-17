---
name: pr-risk-assessment
description: >
  Assess PR or deployment risk against repository risk templates. Use when the
  user asks for a risk rating, questions an existing classification, or requests
  a risk interview. Establish actual use, planned activation, data effects and
  shared-resource impact before recommending a classification.
---

# PR risk assessment

Read `~/.agents/guides/documentation-relevance.md` before producing the assessment.
This is a standalone assessment, not permission to publish, deploy or merge.

## 1. Establish the contract

- Identify the repository, exact diff/revision, intended deployment and scope of
  assessment. Distinguish an individual migration or component from the full PR.
- Read its applicable AGENTS instructions, selected PR template and linked risk
  policy. Prefer the repository's canonical decision tree; do not substitute a
  generic checklist or another repository's template.
- Separate mandatory rules and approval gates from examples qualified by words
  such as “typically”. Preserve qualifiers: **consumed**, **destructive**,
  **existing**, **production**, **irreversible** and **significant** matter.
- If policies conflict, identify the conflict and applicable authority. If the
  required policy or scope is unavailable, do not invent a definitive rating.

## 2. Inspect, then ask

- Inspect the diff, callers, configuration, migrations, worker schedules and
  relevant tests using already-authorized read access. Do not execute live jobs,
  migrations, scans, load tests or rollback experiments merely to assess risk.
- Record what is source-backed, what the user confirms about intended use and
  rollout, and what remains unknown. Carry established answers across related
  PRs unless the scope or evidence changes. Code presence alone is not use.
- Assess the state **after the proposed deployment**, not only today's traffic.
  New functionality enabled at launch is not “unused” merely because it has no
  current consumers. An unused UI can still have active scheduled side effects.
- When an unknown could change the rating or required approval, treat it as an
  unresolved classification gate. Ask one focused question using the harness's
  structured question mechanism, with 2-6 concrete choices and an unknown option
  where useful. Never invent an answer or human approval.
- Use [references/questions.md](references/questions.md) to select the next
  question. Do not administer every question or ask for facts already established.
  If the user does not know, identify the smallest authorized evidence needed;
  leave the rating provisional until the material uncertainty is resolved.

## 3. Make the determination

- Explain the concrete failure mechanism, affected users/data/resources, extent
  of impact and recovery path. Do not raise risk just because a change mentions
  an API, backend, SQL, migration, several files or a large test suite.
- Distinguish an actively depended-on API from an endpoint with unused client
  code. User-confirmed unused functionality reduces exposure, but check shared
  jobs, startup work, external exposure and the planned activation separately.
- Treat creation of a new empty table/index separately from altering populated
  tables, rewriting data, introducing blocking locks or changing live constraints.
  A later feature's possible risk does not automatically belong to the migration.
- Use tests, feature isolation, bounded work and credible rollback to assess
  likelihood and reversibility. They do not erase a mandatory policy trigger,
  prove production behaviour, or make irreversible changes harmless.
- Rate a mixed PR by its highest material applicable risk, not an average of
  file counts. Suggest separation only when the lower-risk part is independently
  deployable; do not split away an inseparable risk to obtain a lower label.
- If a typical example suggests a higher category than the established exposure
  warrants, explain why that example does or does not apply. If a mandatory rule
  still requires the higher category, state both practical exposure and the
  policy-required category. Do not silently override either.

## 4. Return a concise assessment

Give:

1. Scope and recommended category, or **provisional / undetermined**.
2. The applicable template clause and a concrete reason it applies.
3. Material evidence, confirmed usage/rollout conditions and remaining assumptions.
4. Required approvals and what change in scope would require reassessment.

Keep risk, code correctness, CI status, deployment readiness and branch protection
separate. A low-risk label does not satisfy code-owner approval or authorize a
bypass. Do not change labels, PR bodies, credentials or protections without the
relevant authorization. If correcting an earlier rating, acknowledge the mistaken
assumption rather than rationalizing it.

## Validation

When maintaining this skill, exercise the decision and question paths in
[references/validation-cases.md](references/validation-cases.md). These are
fictional tests, not evidence or authorization for a real deployment.
