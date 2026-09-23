# Codex goals for Pi

Ported from OpenAI Codex commit
`13869ca39e7ae749aad2018f4a3a5f67741286d9`, particularly
`codex-rs/ext/goal/src/{spec,tool,steering,runtime,extension,accounting}.rs`
and `codex-rs/state/src/runtime/goals.rs`.
The three templates are unmodified upstream files. The runtime is adapted to
Pi's public extension API. Upstream licensing and notices are included.

Alignment, completion verification and the three-turn semantic blocker rule
are instructions to the working model. There is no independent auditor,
semantic tool gate or arbitrary continuation-count limit.

## Host adaptations

- Pi's session journal replaces the SQLite goal store. Only persistent sessions
  can create goals. Forked snapshots defer continuation until a new user input
  or explicit goal resume; old auditor goals migrate without resuming work.
- `agent_settled` owns automatic continuation, after Pi retries, compaction and
  pending input. Pi's `turn_end` is only a single model/tool round, not a Codex
  goal turn. Escape suppresses continuation without falsely completing a goal.
- The optional planning paragraph refers to `update_plan` or `todo` when present,
  otherwise it is omitted. All objective interpolation is XML-escaped and sent
  as internal user context, not higher-priority instructions.
- Token usage sums Pi's uncached input, cache writes and output. Reported nested
  usage is included. Existing team tools expose cumulative child snapshots, so
  their usage is charged incrementally when observed, not live between polls.
  Unreported child work and compaction model usage are not included in goal
  totals. These are not billing totals or strict expenditure caps.
- Pi does not expose Codex's typed provider-error or execution-handler outcome
  enum. Only explicit final quota/usage-limit messages yield `usage_limited`;
  other final errors yield `blocked`. Only explicit shell spawn errors count
  toward execution-unavailable stopping, not every nonzero command exit.
- Pi has no built-in collaboration Plan mode. This extension does not invent
  one or bypass other extensions' permissions, tool availability or UI gates.

`/goal budget <positive integer|none>` adapts Codex's user-owned budget API.
`--max-goal-token-budget <positive integer>` configures the optional maximum and
default budget. Raising a stopped goal's budget does not itself resume it.
