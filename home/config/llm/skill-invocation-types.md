# Skill invocation types

Before updating this document, read `~/.agents/guides/documentation-relevance.md`.

`skill-catalog.json` is the mechanical source for public skill names, shared and
process locations, invocation classifications, Hermes selection and references.
Home Manager merges it with the private overlay catalogue. The audit script
validates the public catalogue independently. Reference sources are relative to
their LLM root; targets are files under a skill's `references/` directory.

The catalogue is build and audit data, not an additional model-facing registry.
Use `skill_catalog` to discover installed workflows and read the selected
`SKILL.md` for instructions. Private workflows need not exist in public sources.

User-invoked router/orchestrator skills retain explicit-invocation metadata:

- Portable: `agents/openai.yaml` with `allow_implicit_invocation: false`.
- Pi: `disable-model-invocation: true` in `SKILL.md` frontmatter.

Process guardrails remain model-invoked. Classification and discovery do not
override authorization gates: authentication and publication workflows still
require their prescribed approval. Never infer permission from a catalogue match.
