---
name: stimulus
description: >
  Best practices and architecture patterns for using Stimulus in Ruby on Rails applications.
  Covers controller organization, targeting, values, integration with Turbo, and when to reach for Stimulus vs heavier JavaScript.
  Use when building or refactoring frontend behavior in a Rails + Stimulus stack.
---

# Stimulus Best Practices

Stimulus is a lightweight JavaScript framework designed to work *with* the HTML you already have, rather than replacing it. It pairs extremely well with Rails + Turbo.

## Core Philosophy

- **HTML is the source of truth.**
- Keep JavaScript behavior close to the markup it affects.
- Controllers should be small and focused.
- Prefer composition over complex inheritance.

## Recommended Project Structure

```
app/javascript/
├── controllers/
│   ├── application.js
│   ├── hello_controller.js
│   ├── modal_controller.js
│   └── form_controller.js
├── utils/               # Small pure functions
└── channels/            # Action Cable (if used)
```

## Controller Guidelines

### One controller = one concern

**Good**:
- `modal_controller.js`
- `dropdown_controller.js`
- `clipboard_controller.js`

**Bad**:
- `ui_controller.js` (does too many things)

### Use Values for Configuration

```js
// good
static values = {
  url: String,
  method: { type: String, default: "post" }
}

// bad
this.element.dataset.url
```

### Use Targets and Outlets

```js
static targets = ["input", "submit"]
static outlets = ["modal"]
```

This makes dependencies explicit.

## Common Patterns

### Modal / Dialog

```js
export default class extends Controller {
  static targets = ["dialog"]

  open() {
    this.dialogTarget.showModal()
  }

  close() {
    this.dialogTarget.close()
  }
}
```

### Form Handling with Turbo

Combine Stimulus with Turbo Stream responses for excellent UX without heavy JavaScript.

### Real-time Updates

Use Stimulus + Action Cable + Turbo Streams for live updates (chat, notifications, live counters, etc.).

## When to Reach for More Than Stimulus

Use a heavier framework (React, Vue, Svelte, etc.) only when you have:

- Complex client-side state that is hard to keep in sync with the server
- Very rich interactive UIs (spreadsheets, canvas editors, complex drag-and-drop)
- Team expertise and maintenance capacity

For most CRUD + interactive form + real-time notification apps, **Stimulus + Turbo** is the sweet spot in 2025–2026.

## Performance Tips

- Keep controllers small (< 100 lines is a good target).
- Extract shared logic into small utility functions or base controllers.
- Use `requestAnimationFrame` or `debounce` for expensive operations.
- Avoid putting large libraries in Stimulus controllers.

## Integration with Rails

- Use `data-controller`, `data-action`, and `data-*-target` attributes in your ERB/Haml/Slim views.
- Stimulus works beautifully with Turbo Frames and Turbo Streams.
- Consider `stimulus-rails` for easy installation and conventions.

---

This reference will be expanded with more concrete patterns (modals, typeahead, infinite scroll, optimistic UI, etc.) as we build real features.
