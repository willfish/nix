---
name: tui-design
description: Project guide for planning, designing, implementing, and reviewing terminal UI work. Use for Ratatui UI changes, reactive terminal UI architecture, keyboard workflows, colour/theme choices, image preview support, accessibility, TUI issue sequencing, or TUI test planning.
---

# TUI Design

Use this before changing a terminal UI, adding a TUI feature, or writing TUI issues. The default target is a quiet, fast, keyboard-first control surface, not a dashboard or marketing UI.

## Design Principles

- Build for repeated use. Make the primary state and common actions scannable at a glance.
- Prefer dense but calm layouts. Use borders to separate regions, not to decorate every value.
- Keep the first screen functional. Do not add an intro screen, hero panel, or explanatory wall of text.
- Preserve keyboard-first operation. Every common action should be reachable without a mouse.
- Show state, then actions. Important state belongs in the persistent layout; rare guidance belongs in footer/help.
- Use wording that says what happened: `applied: <path>`, `search: 12 results`, `preview unavailable; showing metadata`.
- Treat image preview as progressive enhancement. Metadata and controls must remain useful without graphics support.

## Reactive Architecture

Keep moving toward a Model-Update-View shape:

- The app state is the model. It should contain state needed to render and choose actions, not terminal handles.
- Key and command handling should translate input into explicit actions.
- Domain operations should update the model through a small number of action paths, then reload or patch state deliberately.
- Rendering should be pure enough to snapshot or unit test: avoid filesystem, network, process, or terminal side effects inside draw/render helpers.
- Async/network work should be explicit and bounded. If an action may take time, update the message/status before and after the operation.
- Avoid hidden state duplication. If UI caches derived data, document when it is refreshed.

Preferred future shape for larger changes:

```text
input event -> action -> update model/domain -> render model
```

Do not mix new domain logic into render functions. Do not make rendering depend on real time unless the model carries the time.

## Layout Rules

- Keep top navigation, main content, and footer stable across tabs or modes.
- Use responsive breakpoints rather than shrinking content into unreadable cells.
- At narrow sizes, favor one column and concise metadata.
- At wider sizes, split primary content only when both sides have enough room.
- Never let long paths or labels push controls off screen. Truncate or wrap with intent.
- Lists need stable cursor markers and empty states.
- If a screen cannot fit, show the most important state and keep key commands visible.
- Do not add modal/popup flows until inline command/search modes cannot handle the task clearly.

## Colour And Style

Terminals vary heavily. Design for monochrome first, then add colour as redundant signal.

- Never use colour as the only state indicator. Pair it with text, symbols, ordering, or labels.
- Use a small semantic palette: normal, muted, selected, success, warning, error, accent.
- Prefer terminal defaults for normal text and background.
- If specifying foreground colour, specify a compatible background or avoid assumptions about the user's theme.
- Avoid low-contrast grey-on-black or bright-on-bright combinations.
- Reserve high-intensity colour for short status labels and destructive warnings.
- Use selection styling consistently.
- Make focus obvious in command/search input without relying only on hue.
- Avoid broad colour gradients, decorative colour fields, and brand-heavy palettes in the TUI.

Accessibility target:

- Normal text should meet the WCAG contrast spirit of at least 4.5:1 where colours are controlled.
- Non-text UI cues such as borders, markers, and icons should have at least 3:1 contrast where colours are controlled.
- Because terminal themes are user-controlled, test both dark and light themes and keep a no-colour path legible.

## Design Language

Use a utilitarian product language:

- Prefer domain nouns users already know.
- Prefer verbs that mirror command-line verbs.
- Avoid cute labels and ambiguous metaphors.
- Keep tab titles short.
- Prefer path/status facts over prose.
- Make dangerous actions explicit; they should never look like neutral navigation.
- Use preview to support recognition, not as the whole interface.

## Image Preview

- Support terminals by capability, not by wishful terminal names. Environment hints are useful, but actual protocol detection should win.
- Keep Ghostty/Kitty support first-class through Kitty graphics where available.
- Keep iTerm2 support where the rendering stack can detect it.
- Provide clean fallback copy when previews are disabled, unsupported, too small, or failed to decode.
- Cache decoded/protocol previews by path and render size.
- Never block every frame on image decode. Decode only when path or area changes.
- Keep a reliable environment-variable escape hatch for metadata-only mode.

## Interaction Rules

- `q` should quit from normal mode.
- `Esc` should cancel transient modes.
- `j/k` and arrows should move list selection when lists are focused.
- `Enter` should apply or confirm the current context.
- `:` commands should mirror CLI verbs when practical.
- If a key is destructive or surprising, require clear context and status feedback.
- Footer help should show active-mode keys, not a full manual.

## Implementation Checklist

Before editing:

- Identify whether the change is model, update, view, terminal setup, or preview.
- Choose the smallest module boundary that matches that concern.
- Decide how the UI behaves with no TTY, tiny terminal, no API key/credential, missing current item, no preview protocol, and no colour.

During implementation:

- Keep rendering side-effect free.
- Keep domain effects in model/update methods or explicit action paths.
- Add semantic style helpers before scattering raw colours.
- Use framework widgets and layout constraints rather than hand-built spacing where possible.
- Keep optional preview code behind a feature flag when the dependency stack is optional.

Verification:

- Run formatter and focused TUI tests.
- Run the full relevant test suite.
- Run lint/clippy/static analysis for the touched targets.
- For preview changes, also test the preview feature/dependency flag.
- For terminal behavior, use PTY smoke tests or an interactive manual check in a real terminal.
- For visual/layout changes, inspect dark and light themes, narrow and wide terminal sizes, and preview enabled/disabled states.

## Issue Writing Checklist

Good TUI issues should include:

- User workflow affected.
- Target tab, pane, mode, or screen.
- Current behavior and intended behavior.
- Keyboard behavior.
- Small/narrow terminal behavior.
- Preview-enabled and preview-disabled behavior if relevant.
- Accessibility and colour expectations.
- Tests or manual verification steps.

## References

- Ratatui concepts: https://ratatui.rs/concepts/
- Ratatui application patterns: https://ratatui.rs/concepts/application-patterns/
- Elm commands and subscriptions: https://guide.elm-lang.org/effects/
- WCAG 2.2: https://www.w3.org/TR/wcag/
- WCAG contrast minimum: https://www.w3.org/WAI/WCAG21/Understanding/contrast-minimum.html
