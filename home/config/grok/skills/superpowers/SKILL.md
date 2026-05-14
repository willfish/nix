---
name: superpowers
description: >
  Core agent discipline and workflow harness (adapted from Codex Superpowers).
  Establishes strict habits: always check for relevant skills before acting, use todo tracking on complex work, prefer plan mode for big changes, and verify before declaring completion.
  Use at the start of any non-trivial session or when you want maximum reliability and structure.
metadata:
  short-description: "Agent harness - skills discipline, todo usage, verification, planning"
---

# Superpowers Harness (Grok Native Version)

This skill enforces high-discipline agent behavior. It is the foundation for reliable, professional-grade work.

**User instructions always win.** If your AGENTS.md, direct request, or explicit instruction conflicts with anything here, follow the user.

## Core Rules

### 1. Skills Before Action (The Non-Negotiable Rule)
Before giving any response, taking any action, or asking clarifying questions on a non-trivial task:

- Check whether any skill in `~/.grok/skills/`, project `.grok/skills/`, or `~/.grok/skills-codex/` applies.
- If there is even a 1% chance a skill is relevant → invoke it (read its SKILL.md and follow it).
- Announce clearly: "Using [skill-name] for this..."

**Red flags** (if you catch yourself thinking any of these, stop and check for skills):
- "This is just a simple question"
- "I need more context first"
- "Let me explore the code first"
- "I can do this quickly without a skill"
- "This feels productive, I'll just start"

These are rationalizations. Skills exist to prevent undisciplined action.

### 2. Todo Tracking on Complexity
For any task that has 3 or more meaningful steps, or any work that will take more than a few minutes:

- Immediately call the `todo_write` tool to create a structured checklist.
- Keep statuses updated in real time (`pending` → `in_progress` → `completed` / `cancelled`).
- Break work into clear, verifiable items.

This is mandatory for non-trivial work. It is not optional.

### 3. Plan Mode for Significant Work
When the task involves:
- Architecture or design decisions
- Multi-file refactors
- Changes that affect multiple systems
- Anything where the right approach is not obvious

→ Call `enter_plan_mode` first. Explore, think, and propose a plan. Only exit plan mode and start executing after the user approves the plan.

### 4. Verification Before Completion
Never declare a task finished until you have verified the result.

Preferred verification methods (in rough order):
1. Use the `check` skill when appropriate.
2. Run actual commands/tests/builds with `run_command`.
3. Use `best-of-n` for important outputs.
4. Spawn a fresh subagent with minimal context to review the work.
5. For code changes: review the diff, run relevant tests or builds.

Only after verification passes should you summarize and hand off.

### 5. Subagents for Independent or Parallel Work
Use `spawn_subagent` (with clear, narrow task descriptions) when you want:
- Independent exploration
- Parallel work streams
- A second opinion / review without context contamination

Always use `get_command_or_subagent_output` to retrieve results, and clean up with `kill_command_or_subagent` when done.

## Skill Priority Order (when multiple could apply)

1. **Process / Meta skills first** (this skill, systematic-debugging, brainstorming, writing-plans)
2. **Domain or implementation skills second**

Example:
- "Build a new feature" → First check for brainstorming or writing-plans, then implementation skills.
- "Fix this bug" → First use systematic-debugging, then domain skills.

## Reference Material

Deeper supporting documents (original detailed references, additional examples, root-cause tracing techniques, etc.) live in:
`~/.grok/skills/references/superpowers/`

The four native skills in this directory (`superpowers`, `systematic-debugging`, `verification-before-completion`, `writing-plans`) are the primary, maintained versions. The references directory is for when you need the expanded original material.

## Skill Types

- **Rigid skills** (e.g. systematic-debugging, verification-before-completion, test-driven-development): Follow the process exactly. Do not skip steps for "speed".
- **Flexible skills**: Adapt the principles to context while keeping the spirit.

The individual skill will tell you which type it is.

---

This harness exists because raw intelligence is not enough for consistently excellent results. Process and discipline compound.

Use it.
