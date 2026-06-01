---
name: using-superpowers
description: >
  Use when starting any conversation or before non-trivial work. Establishes how to find and
  invoke skills (Grok /skills injection or Read of SKILL.md) before ANY response, including
  clarifying questions. Pair with superpowers for session discipline.
metadata:
  short-description: "How to invoke skills — mandatory check before acting"
---

# Using Superpowers (Grok Native)

If you were dispatched as a subagent for a narrow task, skip this skill unless the parent asked you to follow the full harness.

## The Rule (Non-Negotiable)

If there is even a **1% chance** a skill applies, you **must** load it **before** any response, tool use, or clarifying question.

- **Grok:** Use `/skills <name>` (injects full content) or **Read** the skill's `SKILL.md` from `~/.grok/skills/<name>/` or project `.grok/skills/`. Never rely on the one-line description alone.
- **Codex / Claude Code:** Use the native Skill tool when available.
- Announce: **"Using [skill-name] for …"**

Rationalizing ("simple question", "need context first", "quick grep first") means stop and load the skill.

## Instruction Priority

1. User explicit instructions (AGENTS.md, direct request) — highest
2. Superpowers / process skills — override default agent behavior
3. Default system prompt — lowest

## Skill Priority (when multiple apply)

1. **Process skills** — `using-superpowers`, `superpowers`, `brainstorming`, `systematic-debugging`, `writing-plans`, `verification-before-completion`
2. **Domain / job skills** — e.g. `hmrc-trade-tariff-workflow`, `rspec-testing`
3. **Implementation references** — under `skills/references/`

Examples:
- "Build X" → `brainstorming` → (approval) → `writing-plans` → implement
- "Fix bug" → `systematic-debugging` → domain skill if needed
- "Done" / PR → `verification-before-completion`

See **AGENTS.md § Skill routing** for the full table.

## Session Bootstrap (Grok)

At the start of non-trivial work, the user may run:

```
/superpowers
/using-superpowers
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

`~/.grok/skills/references/superpowers/skills/using-superpowers/`
