# Adaptive risk questions

Use the repository's template to decide which facts matter. This question bank
supplies prompts, not an alternative policy or a numerical scoring system.
Read `~/.agents/guides/documentation-relevance.md` before writing the result.

## Question rules

Ask only questions whose answers could change the rating, the scope of impact or
an approval requirement. Prefer observed source and established user context to
repeated interviews. Name the actual component and deployment in each question.
Do not equate uncertainty with medium/high risk or confidence with low risk.

Use one structured question at a time. Tailor 2-6 choices to the evidence rather
than copying every option below. Stop when the material facts are sufficient.

## Usage and planned release

**When “consumed API”, “live journey” or “unused functionality” is pivotal:**

“Who depends on this behaviour in the intended release?”

- Active trader/customer workflow.
- Active internal workflow or service.
- Deployed but unused, and remains unused in this release.
- New behaviour that becomes active with this release.
- Unknown; usage or rollout needs checking.

Inspect callers before asking, but do not infer activity from client code alone.
Absence of observed traffic is not proof of no consumers or no external exposure.
Record whose confirmation establishes usage and the scope of that confirmation.

**When a UI is unused but execution may be automatic:**

“Will the changed code run without someone opening this screen?”

- No; it remains inactive until explicitly enabled.
- Yes; a scheduled/background job runs it.
- Yes; startup or deployment runs it.
- Yes; another active request path runs it.
- Unknown.

Check what an off flag actually gates. It might not gate migrations, startup,
worker scheduling or externally reachable routes.

## Data and migrations

“Does this migration change existing data or populated structures?”

- Only creates a new empty table and its indexes, with no automatic population.
- Alters an existing table or index; lock/rewrite behaviour is established.
- Backfills or rewrites existing data.
- Deletes data or changes live constraints/relationships.
- Unknown; inspect the migration and database characteristics.

Ask separately about data size, lock duration or recovery only if they remain
material. A new-table migration and activation of its collection jobs can have
different ratings. Do not assume every rollback must run a destructive down
migration; an unused additive table can often remain in place.

## Shared resources and activation

“What shared work can this operation delay or exhaust?”

- Isolated resources; no active critical consumers share them.
- Shared resources with established bounds and adequate headroom.
- Shared resources with unbounded or unverified load.
- Unknown.

If isolation is described only as separate queues, ask whether the same worker
processes consume them or whether execution capacity is actually reserved.
Queue priority is not pre-emption. An outbound-query limit does not necessarily
limit the threads or connections held by waiting jobs. Check deployment commands,
queue lists and capsules before claiming isolation; distinguish checked-in defaults
from verified runtime configuration.

Inspect concurrency, database connection use, fan-out, scans, retries and memory
where relevant. Name the failure mechanism, not the generic fact that code uses
CPU or a database. Assess planned frequency and release activation, not only a
small local test.

## Exposure, security and recovery

- If the diff appears to change permissions, authentication, credentials,
  retention, regulatory content or production infrastructure, verify the exact
  change against the corresponding template rule. Low usage does not cancel
  an applicable mandatory security or destructive-change rule.
- When reversibility matters, ask whether reverting code leaves data intact,
  requires a tested restore/migration, or leaves an irreversible effect.
- When an API is active, establish compatibility and coordinated deployment
  requirements. Do not call it safe solely because the field is additive, or
  medium solely because the file is a controller.
- If the requested category conflicts with an explicit policy rule, explain the
  conflict. Ask for the unresolved fact or designated policy-owner decision,
  not permission to disguise the change as a lower category.
