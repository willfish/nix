---
name: rspec-testing
description: Use when writing, reviewing, or debugging RSpec tests in Will's Ruby/Rails repos, including model/service/controller specs, FactoryBot setup, flaky test fixes, and Betterspecs-style RSpec conventions.
---

# RSpec Testing

Use this for Ruby/Rails test work.

Read:
- `references/testing.md` for test strategy and when to write tests.
- `references/rspec.md` for detailed RSpec syntax and style.

Defaults:
- RSpec everywhere; no minitest or cucumber.
- Test business logic, bug fixes, API endpoints, models, services, and controllers.
- Prefer test-first for non-trivial logic and bug fixes.
- Use `.method` for class methods and `#method` for instance methods.
- Contexts start with `when`, `with`, or `without`.
- Use factories, not fixtures.
