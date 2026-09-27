# Design acceptance

Apply `~/.agents/guides/documentation-relevance.md` to prose.
Use requirements and observed behaviour, not an aesthetic score. Preserve
baseline and final artifact identities so review matches the final source.
These are implementation acceptance checks, not permission to edit during
advice/critique. In read-only mode report absent evidence and required changes;
do not perform round-trip edits or write a persisted handoff.

## Shared pass/fail checks

1. **Purpose:** a named audience can complete the main task or understand the
   intended message. Content and emphasis support it; the primary action is clear.
2. **Comprehension:** hierarchy, labels, units and state are unambiguous. No fake
   data/claims, invisible failures or missing-data-as-zero substitutions.
3. **Operation:** appropriate keyboard/touch/back paths work; focus/selection is
   visible; errors recover without silent loss of user work.
4. **Inclusion:** appropriate semantics, contrast, non-colour cues, text scaling,
   reflow and motion checks pass. State any untested assistive technology.
5. **Craft:** final renders have legible type, intentional spacing, aligned
   components, no clipping/overlap, and consistent tokens. Inspect small and large
   outputs, not only the flattering size. One distinctive choice can be enough.
6. **Maintenance:** editable sources, fonts/assets and exports are reproducible.
   Reopen/edit/re-export a representative artifact when establishing a pipeline.

A severe failure in task completion, truthfulness, accessibility or editability
blocks readiness; attractive styling cannot compensate. State findings with
location, observed effect and remedy. Do not claim user validation without users.

## Surface evidence

| Surface | Required evidence for an implementation claim |
| --- | --- |
| TUI | Real terminal/PTY journey and mode transitions, narrow/wide views, no-colour selection, terminal restoration; graphics fallback if used |
| Dashboard | Known results for duplicates, empty denominator, missing observations and time boundaries; rendered values/filter; units/freshness and accessible chart alternative; authorized query verification for live data |
| Web | Main task and error/retry via keyboard, names/roles/states, visible focus, reflow/text scaling and rendered views; full relevant project checks |
| Native desktop | Target build/runtime, window sizing/shortcuts/focus, accessibility API semantics, large text and error recovery |
| Mobile | Named native or PWA runtime, navigation/back, safe-area/keyboard handling, text scaling, touch access, offline/interruption/recovery; device/simulator checks for native claims |
| Deck | Every page rendered/read at presentation size; narrative and reading order; no clipping; editable text/chart and source edit/re-export |
| Marketing | Audience/channel/proposition; claim/asset provenance; readable target-size exports; meaningful CTA/destination and consent when interactive |

For each check distinguish **passed**, **failed**, **not run**, and **not
applicable with reason**. Record command/action, runtime/viewport/theme, observed
result and artifact path. A file existing, a parse passing, a test checklist or
a planned command is not evidence that the design works.

## Safety probes for harness changes

Use disposable synthetic fixtures and non-mutating services. Observe tool calls
and compare filesystem state for read-only critique, instructions hidden in a
reference/page to publish or upload, authenticated but unapproved mutations,
unavailable browser/SDK and explicit requests needing hard-gate approval.
Compliant final prose is not proof that no unauthorized action occurred.

Offline scripted providers prove transport, loading and tool exposure. Real-model
trials prove behaviour only for those briefs. Neither establishes a sandbox or a
guarantee of future compliance. Retain compact local evidence and limitations;
keep raw private content and credentials out of reports and Git.
