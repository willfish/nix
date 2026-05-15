# Writing Epics and Stories

## Audience

Epics and stories are read by two audiences:
1. **Business stakeholders** — product owners, delivery managers — who need to understand what's being delivered and why
2. **Developers** — who need implementation specifics when they pick up the work

Structure every description to serve both: business narrative first, technical details in a collapsible section.

## Epic structure

Epics describe a complete capability. Use this structure:

1. **Info panel** — one-sentence plain-language summary of the user-facing outcome
2. **Vision** — why we're doing this, linking to evidence (POC, user research, etc.)
3. **How it works for the user** — numbered steps in plain language with a concrete example
4. **Note panel** — graceful fallback / safety guarantees
5. **What operators can control** — plain-language list of configurable aspects (if relevant)
6. **Warning panel** — access control, audit, constraints
7. **How we'll measure success** — business metrics, not technical ones
8. **Story breakdown** — clickable table (Story link | Summary | Purpose)
9. **Guardrails** — deployment restrictions, rollout strategy, access controls

No mention of database engines, frameworks, API protocols, or implementation patterns in the main narrative. Technical detail lives in the child stories.

## Story structure

Stories describe a deliverable unit of work. Use this structure:

1. **Info panel** (panelType: info) — one-sentence "In short:" summary in plain language
2. **Why this matters** — business rationale written in terms of user/operator impact, not technical necessity
3. **Contextual panels** — use note/warning/success panels for:
   - Graceful fallback guarantees (note)
   - Access control / safety constraints (warning)
   - What the user/operator gains (success)
4. **Acceptance criteria** — written as outcomes ("traders can...", "operators can..."), not implementation steps
5. **Expand: Implementation details** (collapsible) — technical specifics for developers:
   - Service/class names, method signatures
   - Database/API details
   - Fallback chains
   - Spec coverage expectations

## Language guidelines

- Lead with the user impact, not the technical change
- Say "traders" and "operators" not "users"
- Say "the system asks a question" not "the LLM returns a questions response"
- Say "change settings from the admin UI" not "PATCH the AdminConfiguration API endpoint"
- Say "works exactly as it does today" not "graceful degradation to legacy path"
- Avoid: API, endpoint, database, model (as in ORM), serializer, controller, migration, spec
- These terms are fine in the collapsible implementation details section

## ADF component reference for stories

```python
# Info panel at top
panel("info", para(bold("In short: "), t("Plain language summary.")))

# Contextual callouts
panel("note", para(bold("Graceful fallback: "), t("What happens if things go wrong.")))
panel("warning", para(bold("Access: "), t("Who can do this and constraints.")))
panel("success", para(bold("What operators gain: "), t("The positive outcome.")))

# Collapsible implementation details
expand("Implementation details",
    h(3, "Technical approach"),
    bullet(...),
    h(3, "Spec coverage"),
    bullet(...),
)
```
