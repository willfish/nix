---
name: rspec-testing
description: RSpec test work. Use for Ruby/Rails specs, model/service/controller tests, FactoryBot, flaky specs, debugging failures, and Better Specs conventions.
---

# RSpec Testing

Use this for Ruby/Rails test work.

Read:
- `references/testing.md` for test strategy and when to write tests.
- `references/rspec.md` for the Better Specs-derived overview and checklist.
- `references/rspec-structure.md` for describe/context shape, expectations, naming, and shared examples.
- `references/rspec-factories.md` for FactoryBot, fixtures, minimal records, and database setup.
- `references/rspec-rails.md` for request/model/service boundaries, external HTTP stubs, and service object specs.
- `references/rspec-flakiness.md` for focused runs, final verification, formatter output, and flaky tests.

Defaults:
- RSpec everywhere; no minitest or cucumber.
- Test business logic, bug fixes, API endpoints, models, services, and controllers.
- Do not use TDD. Implement first, then add tests. For bugs, add a regression test.
- Use `.method` for class methods and `#method` for instance methods.
- Contexts start with `when`, `with`, or `without`.
- Cover valid, edge, and invalid cases.
- Test observable behavior; avoid controller internals.
- Stub external HTTP rather than calling live services.
- Use factories, not fixtures.
