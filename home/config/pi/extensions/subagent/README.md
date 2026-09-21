# Pi specialist teams

This adapts the subagent example shipped with the pinned Pi 0.85.1 runtime. `index.ts` retains its headless runner and adds resumable interactive single/parallel/chain jobs; `agents.ts` retains its agent discovery. Compare both with the pinned example when upgrading Pi.

## Start a team

In a standard Pi session inside herdr, request only the roles that add distinct value. For a change needing design, implementation and independent review, for example:

> Have the architect investigate the design, then give the builder an approved implementation task with explicit file ownership. Load local-dev-environment for Nix work. Ask the sceptic to review the resulting diff independently.

The main Pi coordinates. Children have real interactive Pi terminals and isolated conversations, but share filesystem access. They are not sandboxes. Questions and answers stay in the main pane; entering a child pane is optional.

Personas are ordinary agent Markdown files:

| Persona | Perspective | Required skills |
| --- | --- | --- |
| architect | Read-only design and trade-offs | chain-of-verification |
| builder | Implementation within assigned file ownership | verification-before-completion |
| sceptic | Read-only independent review | code-review-workflow |
| test-engineer | Minimal executable regressions derived from requirements | Task-specific only |
| security-reviewer | Read-only analysis of a named trust boundary | Task-specific only |
| domain-specialist | Source-backed business rules and acceptance examples | Task-specific only |

Work solo unless delegation answers a distinct question or the user requests a team. These are available roles, not a checklist: use at most one general reviewer, adding specialists only for separate evidence or artifacts. Four panes are a capacity limit, not a staffing target. Assign non-overlapping ownership to the builder and test engineer.

The domain specialist is for conflicting or unclear business rules. Use scout plus a domain skill for ordinary domain-code reconnaissance. Choose framework, domain and security skills for the task rather than loading them into every specialist. Role bodies load only when dispatched; normal Pi instructions and skill discovery still apply. Children still load shared `AGENTS.md` context files. Orchestrator-only rules live in `~/.pi/agent/ORCHESTRATOR.md` and are appended only when `PI_TEAM_CHILD` is unset. Concise handoffs keep parent context focused.

Existing scout, planner, worker and reviewer definitions still work. The latter three are compatibility roles, not additional team stages.

## Model and thinking

Each role pins defaults in agent frontmatter. Those are not listed in the coordinator prompt or `subagent` tool schema. Dispatch uses the role, then the coordinator session. Thinking values are `off`, `minimal`, `low`, `medium`, `high`, `xhigh` and `max`. YAML 1.1 treats unquoted `off` as false; the loader accepts that spelling.

```yaml
model: xai/grok-4.7
thinking: medium
```

## Skills

Declare persona defaults in agent frontmatter:

```yaml
skills: [verification-before-completion, rspec-testing]
```

The `subagent` tool also accepts `skills` for a single call, a parallel task or a chain step. Top-level skills apply to every task in that call. Defaults and additions are deduplicated and their full SKILL.md contents are loaded before launch, with absolute reference base directories. Missing or malformed declarations fail rather than silently skipping a workflow.

Skills resolve against the coordinator's active Pi skill registry, including its project trust decisions. Other task-relevant skills remain discoverable in the child. Skill instructions do not restrict operating-system permissions. Project agent prompts retain the upstream confirmation flow.

## Follow-up and intervention

Each result includes a member ID. Both the `team` tool and `/team` command support:

```text
/team list
/team read <id>
/team send <id> <follow-up task>
/team steer <id> <guidance>
/team close <id>
/team close all
```

`send` keeps the child's context in a follow-up job. `steer` acknowledges queued guidance, not completion, and cannot bypass a pending question. `read` accepts a member or job ID; member reads include direct user conversations. Collaboration is routed through the coordinator; there is no peer messaging bus.

## Questions in the main pane

A child calls `ask_coordinator` when it needs clarification, with at least two concrete choices. Its execution segment ends with a structured multiple-choice question, not a completed task. The coordinator receives the first settled question without waiting for slow siblings, while the job retains running and queued work.

The coordinator uses `team answer` from evidence or an architect recommendation. Human hard-gate questions with choices open a selectable list in this main pane on wait, not a typed chat prompt. Use `team ask` for the same list. Design and plan choices are not human questions. Questions marked `requiresUser` accept answers only through that selectable list or a user slash-answer, never a model-supplied provenance flag. Dismissing or cancelling the dialog leaves the question pending.

```text
/team questions
/team answer <question-id> <answer>
/team ask <question-id>
/team wait <job-id>
/team read <job-id>
/team cancel <job-id>
```

An answer acknowledges queued input, then resumes the same child conversation. `wait` returns the next question, final job result or a progress snapshot within 30 seconds. Later questions and completion are also notified in the main conversation. Chained steps advance only on final output; answering a question never substitutes its text for `{previous}`. Do not restart a paused chain.

Pending questions keep their capacity slots. A new job blocked behind other questions returns their IDs instead of trapping the coordinator. Answer those first. Esc cancels a job while its dispatch/wait call is attached; after it yields, use explicit `cancel`. If pane cleanup fails, its slot stays reserved and the coordinator receives the failure; retry `close <member-id>` to release it safely.

Call blocking parent delegation/wait/ask tools alone, using the `tasks` array for parallelism. Children must call the question tool alone too. Mixed tool batches are blocked before sibling effects. Pending questions cannot be bypassed through ordinary steering or child-pane input. Question/resume support is interactive-herdr-only; the headless runner is unchanged.

## Pane lifecycle

- Children open to the right without changing focus. Only their downward-split column is rebalanced; the coordinator's width is preserved.
- Session/Switchboard labels are set at launch from the role and current task (`pi --name` first on the child argv, plus the Switchboard `agent-bus-label` entry). After spin-up the child reapplies that label from `request.json`, then refreshes it on follow-up prompts. This does not wait for the child model to call `set_agent_label`.
- Four panes are retained. At capacity, an idle child must acknowledge retirement before being replaced. Busy conversations, pending questions and incomplete cancellation cleanup are not eligible.
- Completed panes stay open for follow-up. Closing, evicting, or shutting down the parent closes only owned child panes. Normal Pi session files remain available for resumption.
- Startup is limited to 60 seconds and each execution segment to 30 minutes. Human answer time does not consume that limit; leases and health checks continue. Dispatch/job waits yield within 30 seconds without cancelling background work. Explicit cancellation or a failed execution wait requests abort, then closes the owned pane.
- A coordinator lease expires after 30 seconds without renewal. Children abort and request shutdown, with a bounded owned-pane close fallback. Private IPC ownership records remain if the coordinator dies unexpectedly.
- Child reload/session replacement ends an outstanding request; use a new dispatch rather than transferring its bridge to another conversation.
- Outside interactive herdr, delegation retains the upstream headless JSON runner with the same skill preloading. The memory-constrained local Qwen launcher still excludes this extension.
- Team children set `PI_OM_DEFAULT=0`. They compact with Pi's normal summariser, do not inherit coordinator observational memory, and do not run Jev. Compact still must not inject a resume turn if om is later turned on in that pane.

Herdr layout edits use positional paths and are not transactional across clients. Avoid moving/splitting panes while a team is being created or balanced. Mixed ownership detected in a layout disables rebalancing, but the public API cannot rule out a simultaneous manual topology change between validation and a ratio write.

## Implementation boundaries

- `skills.ts`: strict declarations and prompt composition.
- `labels.ts`: role/task session names used as Switchboard presence labels.
- `launch.ts`: role model/thinking defaults and coordinator overrides.
- `herdr.ts`: bounded socket calls, explicit pane ownership and subtree ratios.
- `protocol.ts`: private atomic command/result files.
- `child.ts`: readiness, multiple-choice questions, validated answer delivery, settlement, retirement and coordinator lease.
- `team.ts`: parent lifecycle, answer transport, retained sessions and `PI_TEAM_CHILD` for headless and interactive children.
- `jobs.ts`: session-owned scheduling, question tracking, continuation and cancellation capacity.
- `job-results.ts`: honest waiting/result rendering, segment usage and parent batch guards.
- `controls.ts`: coordinator tool, slash command and collaboration guidance.

## Checks

```sh
direnv exec . node --test tests/pi-*.test.ts tests/local-pi.test.ts
# Optional live test: temporary owned panes, local mock model, no paid requests.
direnv exec . env PI_TEAM_LIVE_TEST=1 node --test --test-concurrency=1 tests/pi-team-runtime.test.ts tests/pi-team-question-runtime.test.ts
```
