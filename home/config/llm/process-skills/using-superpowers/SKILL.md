---
name: using-superpowers
description: >
  Skill invocation discipline. Use before non-trivial work or clarifying
  questions to find and load the right skills before responding.
metadata:
  short-description: "How to invoke skills — mandatory check before acting"
---

# Using Superpowers

If you were dispatched as a subagent for a narrow task, skip this skill unless the parent asked you to follow the full harness.

## The Rule (Non-Negotiable)

If there is even a **1% chance** a skill applies, you **must** load it **before** any response, tool use, or clarifying question.

- Use the active harness catalogue to discover skills, then read the full `SKILL.md`. Pi supports `/skill:<name>` and shared `~/.agents/skills/` or project `.agents/skills/`. Never rely on the one-line description alone.
- Announce: **"Using [skill-name] for …"**

Rationalizing ("simple question", "need context first", "quick grep first") means stop and load the skill.

## Instruction Priority

Follow system and developer instructions before user requests and project guidance.
Skills supply task procedures; they do not override higher-priority instructions.

## Skill Priority (when multiple apply)

1. **Process skills** — `using-superpowers`, `superpowers`, `brainstorming`, `systematic-debugging`, `writing-plans`, `verification-before-completion`
2. **Domain / job skills** — e.g. `pull-request-workflow`, `rspec-testing`
3. **Implementation references** — under `skills/references/`

Examples:
- "Build X" → `brainstorming` → (approval) → `writing-plans` → implement
- "Fix bug" → `systematic-debugging` → domain skill if needed
- "Done" / PR → `verification-before-completion`

See **AGENTS.md § Skill routing** for the full table.

## Session Bootstrap

At the start of non-trivial work, the user may run:

```
/skill:superpowers
/skill:using-superpowers
```

You should treat that as mandatory harness activation for the session.

## Red Flags

| Thought | Reality |
|---------|---------|
| "Just a simple question" | Questions are tasks — check skills |
| "I need context first" | Skills define *how* to gather context |
| "I remember this skill" | Re-read current `SKILL.md` |
| "Skill is overkill" | Use it anyway |

## Full Reference

Expanded flowcharts, platform tool mappings, and upstream wording:

`~/.agents/references/superpowers/skills/using-superpowers/`
