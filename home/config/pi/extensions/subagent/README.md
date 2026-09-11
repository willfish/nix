# Pi specialist teams

This adapts the subagent example shipped with the pinned Pi 0.85.1 runtime. `index.ts` retains its single/parallel/chain orchestration and result rendering; `agents.ts` retains its agent discovery. Compare both with the pinned example when upgrading Pi.

## Start a team

In a standard Pi session inside herdr, ask:

> Have the architect investigate the design, then give the builder an approved implementation task with explicit file ownership. Load local-dev-environment for Nix work. Ask the sceptic to review the resulting diff independently.

The main Pi coordinates. Children have real interactive Pi terminals and isolated conversations, but share filesystem access. They are not sandboxes. Enter any child pane to ask questions or steer its work; return to the coordinator to integrate the result.

Personas are ordinary agent Markdown files:

| Persona | Perspective | Required skills |
| --- | --- | --- |
| architect | Read-only design and trade-offs | chain-of-verification |
| builder | Implementation within assigned file ownership | verification-before-completion |
| sceptic | Read-only independent review | code-review-workflow, verification-before-completion |

Existing scout, planner, worker and reviewer definitions still work. Model and thinking level inherit from the coordinator unless the agent pins a model.

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

`send` keeps the child's context and waits for a new result. `steer` acknowledges queued guidance, not completion of that guidance. `read` retrieves the latest settled result, including direct user conversations. Collaboration is routed through the coordinator; there is no peer messaging bus.

## Pane lifecycle

- Children open to the right without changing focus. Only their downward-split column is rebalanced; the coordinator's width is preserved.
- Four panes are retained. At capacity, an idle child must acknowledge retirement before being replaced. A busy manual conversation is not eligible.
- Completed panes stay open for follow-up. Closing, evicting, or shutting down the parent closes only owned child panes. Normal Pi session files remain available for resumption.
- Startup is limited to 60 seconds and each awaited task to 30 minutes. Cancellation and failed waits request abort, then close the owned pane.
- A coordinator lease expires after 30 seconds without renewal. Children abort and request shutdown, with a bounded owned-pane close fallback. Private IPC ownership records remain if the coordinator dies unexpectedly.
- Child reload/session replacement ends an outstanding request; use a new dispatch rather than transferring its bridge to another conversation.
- Outside interactive herdr, delegation retains the upstream headless JSON runner with the same skill preloading. The memory-constrained local Qwen launcher still excludes this extension.

Herdr layout edits use positional paths and are not transactional across clients. Avoid moving/splitting panes while a team is being created or balanced. Mixed ownership detected in a layout disables rebalancing, but the public API cannot rule out a simultaneous manual topology change between validation and a ratio write.

## Implementation boundaries

- `skills.js`: strict declarations and prompt composition.
- `herdr.js`: bounded socket calls, explicit pane ownership and subtree ratios.
- `protocol.js`: private atomic command/result files.
- `child.js`: readiness, delivery, settlement, retirement and coordinator lease.
- `team.js`: parent lifecycle and retained sessions.
- `controls.js`: coordinator tool, slash command and collaboration guidance.

## Checks

```sh
direnv exec . node --test tests/pi-*.test.mjs tests/local-pi.test.mjs
# Optional live test: temporary owned panes, local mock model, no paid requests.
direnv exec . env PI_TEAM_LIVE_TEST=1 node --test tests/pi-team-runtime.test.mjs
```
