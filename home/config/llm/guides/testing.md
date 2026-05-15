# Testing

## Stack

- **Unit/integration tests**: RSpec (all Ruby repos)
- **End-to-end tests**: Separate repo (`trade-tariff-e2e-tests`)
- **No minitest, no cucumber** — RSpec everywhere

## When to write tests

**Always write tests when:**
- Adding a new model, service, or controller action
- Changing business logic (anything that affects what users see or data integrity)
- Fixing a bug (write the failing test first, then fix)
- Adding a new API endpoint

**Skip tests when:**
- Pure config changes (routes, env vars, Terraform)
- Updating dependencies (Dependabot PRs)
- Documentation-only changes

## What to test

### Models
- Validations
- Associations
- Scopes
- Any custom methods

### Services
- Happy path
- Edge cases
- Error handling
- Return values

### Controllers / API endpoints
- Status codes
- Response shape
- Auth/permissions
- Error responses

### Don't test
- Framework behaviour (Rails already tests `has_many`)
- Private methods directly (test through the public interface)
- Trivial getters/setters

## RSpec conventions

See `rspec.md` for syntax. Key points:

- `.method` for class methods, `#method` for instance methods
- Contexts: "when", "with", "without"
- Descriptions under 40 chars
- One assertion per example where practical
- Use `let` and `let!` — avoid `before` blocks for setup that `let` handles
- Use factories (FactoryBot), not fixtures

## Test-first for non-trivial logic

For anything algorithmic or with complex branching:
1. Write the test that defines success
2. Run it — confirm it fails
3. Implement until it passes
4. Refactor with confidence

Tests are your loop condition. Use them.

## Flaky tests

If you encounter a flaky test:
1. Identify the source (timing, ordering, shared state)
2. Fix it properly — don't just add `sleep` or retry
3. If you can't fix it immediately, note it in the PR description
