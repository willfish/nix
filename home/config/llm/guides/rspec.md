# RSpec Best Practices

Source: https://www.betterspecs.org/
Checked: 2026-06-20
Update trigger: RSpec/Rails major release, Better Specs guidance change, local test workflow failure, or quarterly review.

Rules distilled from Better Specs and adapted for Will's Ruby/Rails repos. Use
this as the overview, then load the narrow branch reference for the task.

## Branch References

- `references/rspec-structure.md` for describe/context shape, expectations,
  shared examples, naming, and general RSpec style.
- `references/rspec-factories.md` for FactoryBot, minimal records, fixtures,
  and updating records in specs.
- `references/rspec-rails.md` for request/model/service boundaries, external
  HTTP stubs, rake output helpers, and service object examples.
- `references/rspec-flakiness.md` for focused runs, final verification,
  formatter output, live-service avoidance, and flaky test work.

## Better Specs Checklist

- Describe methods with `.method` for class methods and `#method` for instance
  methods.
- Use `context` blocks for conditions. Start them with `when`, `with`, or
  `without`.
- Keep example descriptions short, ideally under 40 characters.
- Specify one behavior per example where practical; use `aggregate_failures`
  only for tightly related attributes from the same behavior.
- Cover valid, edge, and invalid cases.
- Use `expect` and `is_expected.to`, not `should`.
- Prefer named `subject` and `let` over instance variables and setup-heavy
  `before` blocks.
- Mock sparingly. Prefer real behavior unless an external service, expensive
  dependency, or hard-to-trigger branch makes a stub clearer.
- Create only the records the example needs.
- Use factories, not fixtures.
- Test observable behavior and avoid controller internals.
- Stub external HTTP with WebMock or VCR.
- Run focused tests while iterating, then the full relevant group before
  claiming completion.
