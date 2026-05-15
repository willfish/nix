---
name: rspec-testing
description: >
  Use when writing or reviewing RSpec tests in the trade-tariff Rails
  applications, especially decisions about what to test, test-first for
  non-trivial logic, RSpec conventions, and how testing fits into the
  broader trade-tariff development workflow.
metadata:
  short-description: "RSpec testing strategy and conventions for trade-tariff work"
---

# RSpec Testing (Trade Tariff)

Use this for testing work in the trade-tariff Rails repos.

**Read first:**
- `references/testing.md` — When to write tests, what to test (models, services, controllers), what not to test, test-first for algorithmic work, how to handle flaky tests.
- `references/rspec.md` — Syntax conventions (describe `.method` / `#method`, contexts starting with "when/with/without", single expectation per test, `aggregate_failures`, `let` over `before`, etc.).

Note: This complements the more Sequel-specific patterns in the `rspec-sequel` skill. Use both when working on trade-tariff codebases.