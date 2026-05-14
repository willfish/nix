# Common Stimulus Patterns

## Modal / Dialog Controller

```js
export default class extends Controller {
  static targets = ["dialog"]

  open() { this.dialogTarget.showModal() }
  close() { this.dialogTarget.close() }
}
```

Usage:
```html
<div data-controller="modal">
  <button data-action="modal#open">Open</button>
  <dialog data-modal-target="dialog">...</dialog>
</div>
```

## Dropdown / Popover

Use the `data-action` with `click->dropdown#toggle` and `click@window->dropdown#hide`.

## Clipboard

```js
export default class extends Controller {
  static values = { text: String }

  copy() {
    navigator.clipboard.writeText(this.textValue)
    // show "Copied!" feedback
  }
}
```

## Debounced Input (Search / Typeahead)

```js
export default class extends Controller {
  static targets = ["input"]

  search() {
    clearTimeout(this.timeout)
    this.timeout = setTimeout(() => {
      // perform search or Turbo visit
    }, 300)
  }
}
```

## Optimistic UI Updates

Update the DOM immediately, then handle success/failure from the server response (especially easy with Turbo Streams).

## Form Submission with Loading State

```js
submitStart() { this.element.disabled = true }
submitEnd() { this.element.disabled = false }
```

## Key Takeaways

- Keep controllers focused on **one UI component**.
- Use Values for configuration, Targets for elements, Outlets for cross-controller communication.
- Combine Stimulus with Turbo Streams for the best of both worlds (server-rendered HTML + light client behavior).
