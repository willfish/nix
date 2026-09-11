---
name: create-skill
description: Skill creation and updates. Use when creating, scaffolding, or revising a skill, SKILL.md, references, scripts, agents metadata, or /create-skill output.
---

# Create or update a skill

Before drafting prose, read `~/.agents/guides/documentation-relevance.md`.
Read the active harness's installed skill documentation before changing its
loading or invocation contract.

## Agree the design

For a new skill, establish its name, scope, purpose and invocation triggers.
Ask missing questions one at a time. For an update, inspect the existing skill
and preserve its authorization gates, manual-only triggers and secret protections.
Show the proposed description and workflow and obtain approval before implementation.

- Use lowercase hyphenated names, 2 to 64 characters.
- For shared dotfiles skills, edit the canonical `home/config/llm/` source,
  not Home Manager's generated links.
- For standalone skills, use `~/.agents/skills/<name>/SKILL.md` or
  project `.agents/skills/<name>/SKILL.md` as appropriate.
- Name precise "use when" triggers in the description. Discovery never
  authorizes the skill's actions.

## Implement

Create the directory and a `SKILL.md` with `name` and `description` frontmatter.
Keep the body concise, portable and imperative. Do not duplicate general
harness instructions. Every prose-producing skill or guide must reference
`~/.agents/guides/documentation-relevance.md` instead of copying its rules.

Use progressive disclosure:

- `SKILL.md`: workflow, decisions, gates and navigation.
- `references/`: details loaded only when relevant.
- `scripts/`: repeatable logic, with automated tests.
- `assets/`: templates used to produce outputs.

Preserve existing explicit-invocation metadata when updating manual-only skills.
Do not turn a user-invoked workflow into an automatic action.

## Verify

Read back the files. Validate names, frontmatter, references and deployment.
Run `python3 home/config/llm/scripts/audit-skills` in the dotfiles environment.
Test realistic positive and negative trigger scenarios, authorization gates and
helper scripts. Use isolated fixtures and clean up artifacts. Report observed
results and any blockers, not inferred success.

Pi supports explicit invocation through `/skill:<name>` and shared skill
loading through `~/.agents/skills/`. In this dotfiles harness, discover skills
with `skill_catalog`, then read the selected absolute `SKILL.md` before use.
