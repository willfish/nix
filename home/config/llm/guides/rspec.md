# RSpec Best Practices

Rules distilled from [Better Specs](https://www.betterspecs.org/) and adapted
for Will's Ruby/Rails repos.

## Better Specs checklist

When writing or reviewing RSpec, check these first:

- Describe methods with Ruby documentation syntax: `.method` for class methods,
  `#method` for instance methods.
- Use `context` blocks for conditions. Start them with `when`, `with`, or
  `without`.
- Keep example descriptions short, ideally under 40 characters. Move detail
  into contexts.
- Specify one behavior per example where practical. Use `aggregate_failures`
  only for tightly related attributes from the same behavior.
- Cover valid, edge, and invalid cases. Think in inputs, permissions, missing
  records, ownership, and error states.
- Use `expect` and `is_expected.to`, not `should`.
- Prefer named `subject` and `let` over instance variables and setup-heavy
  `before` blocks.
- Mock sparingly. Prefer real behavior unless an external service, expensive
  dependency, or hard-to-trigger branch makes a stub clearer.
- Create only the records the example needs. If dozens of rows seem necessary,
  reconsider the design or fixture shape.
- Use factories, not fixtures. For unit tests, push domain logic into code that
  can be tested without factories when possible.
- Use readable matchers. Prefer `expect { action }.to raise_error(...)` and
  domain-specific matchers over opaque boolean checks.
- Use shared examples for repeated behavior such as auth, pagination, response
  shape, and common error handling.
- Test observable behavior: request specs, integration behavior, models, and
  services. Avoid controller internals.
- Stub external HTTP with WebMock or VCR. Do not depend on live services.
- Use focused test runs during development, then run the full relevant group
  before claiming the work is complete.
- Treat preloaders and faster formatters as workflow aids only. They do not
  replace focused tests, design clarity, or final verification.

## Describe methods

Use `.` for class methods, `#` for instance methods:

```ruby
# Good
describe '.authenticate' do
describe '#admin?' do

# Bad
describe 'the authenticate method for User' do
```

## Use contexts

Start contexts with "when", "with", or "without":

```ruby
context 'when logged in' do
context 'with valid params' do
context 'without authentication' do
```

## Keep descriptions short

Under 40 characters. Split with contexts if needed:

```ruby
# Good
it 'returns the user' do

# Bad (43 chars - too long)
it 'returns the single result with strong confidence' do

# Better - move detail to context
context 'with a single result' do
  it 'returns strong confidence' do
```

## Single expectation per test

One behavior per test. Multiple expectations usually mean the example is
covering multiple behaviors.

Use `aggregate_failures` when checking related attributes from the same result:

```ruby
# Good - single expectation
it 'returns status 200' do
  expect(response.status).to eq(200)
end

# Good - related attributes with aggregate_failures
it 'returns the user attributes', :aggregate_failures do
  expect(result.name).to eq('Test')
  expect(result.email).to eq('test@example.com')
end

# Avoid - invoke then check mock (split these)
it 'calls the AI client' do
  result  # invokes subject
  expect(OpenaiClient).to have_received(:call)
end
```

For slower request, database, or end-to-end style specs, grouping closely
related assertions can be a reasonable trade-off when repeating the full setup
would make the suite materially slower.

## Test all cases

Cover happy path, edge cases, and errors. Include missing records, unauthorized
access, invalid input, ownership boundaries, and empty result sets:

```ruby
describe '#destroy' do
  context 'when resource exists' do
  context 'when resource is missing' do
  context 'when user lacks permission' do
end
```

## Use expect syntax

```ruby
# Good
expect(actual).to eq(expected)
is_expected.to be_valid  # for one-liners

# Avoid
actual.should eq(expected)
```

## Use subject and let

```ruby
subject(:user) { described_class.new(attrs) }

let(:attrs) { { name: 'Test' } }
let!(:eager_loaded) { create(:thing) }  # use let! sparingly
```

Use `let!` only when the record must exist before the subject runs, for example
when testing a query or scope. Otherwise prefer lazy `let`.

## Avoid excessive mocking

Test real behavior when practical. Mocks speed up tests but add maintenance
burden and can hide integration failures.

Good uses of stubs:
- Simulating hard-to-reach branches such as a dependency returning 404.
- Replacing external HTTP calls.
- Isolating a slow or non-deterministic dependency.

Avoid stubbing the object under test or restating its implementation through
mock expectations.

## Create minimal test data

Only what's needed:

```ruby
# Good
let(:user) { create(:user) }

# Bad - loading unnecessary data
let(:users) { create_list(:user, 50) }
```

## Use factories over fixtures

```ruby
# Good
let(:user) { create(:user, :admin) }

# Avoid
fixtures :users
```

For isolated unit tests, prefer plain Ruby objects or explicit value objects
over factories when persistence is not part of the behavior.

## Use readable matchers

Use RSpec's expressive matchers instead of manual boolean checks:

```ruby
# Good
expect { model.save! }.to raise_error(Sequel::ValidationFailed)
expect(response).to have_http_status(:not_found)
expect(payload).to include('error')

# Avoid
expect(model.valid?).to eq(false)
```

## Naming conventions

Avoid "should" in descriptions:

```ruby
# Good
it 'returns nil' do
it 'raises an error' do

# Bad
it 'should return nil' do
```

## Shared examples

DRY up repeated test patterns. Place in `spec/support/shared_examples/`:

```ruby
# spec/support/shared_examples/ai_client_examples.rb
RSpec.shared_examples 'does not call the AI client' do
  it 'does not call the AI client' do
    allow(OpenaiClient).to receive(:call)
    subject
    expect(OpenaiClient).not_to have_received(:call)
  end
end

# In specs:
context 'when query is blank' do
  let(:query) { '' }

  it_behaves_like 'does not call the AI client'
end
```

Common candidates for shared examples:
- API response format checks
- Authentication/authorization behavior
- Pagination behavior
- Error handling patterns

## Test observable behavior

Prefer specs that exercise the public boundary:

- Request specs for API endpoints.
- Model and service specs for business behavior.
- Integration/system specs where the user-visible flow matters.

Do not add new controller specs. When touching old controller specs, migrate the
coverage into request specs and remove the controller spec.

## Stub external requests

Use WebMock or VCR for HTTP calls:

```ruby
before do
  stub_request(:get, 'https://api.example.com/data')
    .to_return(body: '{"result": "ok"}')
end
```

Never let specs depend on live third-party services. Live dependencies make
tests flaky, slow, and hard to reproduce.

## Focused runs and final verification

Run focused specs while iterating:

```sh
bundle exec rspec spec/services/my_service_spec.rb
bundle exec rspec spec/requests/api/v2/widgets_spec.rb:42
```

Before claiming a change is ready, run the full relevant group from the
worktree. For example:

```sh
bundle exec rspec spec/services spec/requests
```

If a preloader or watcher exists in the repo, use it only as a local speed aid.
The final verification command should be a direct command whose output can be
reported.

## Formatter output

Use formatter output that makes failures easy to read. Do not change a repo's
formatter casually; follow the local `.rspec`, CI, and team conventions.

## Suppress noisy output

Use the `suppress_output` helper (from `spec/support/output_helpers.rb`) for rake tasks:

```ruby
subject(:seed) do
  suppress_output { Rake::Task['db:seed'].invoke }
end
```

## Updating records in specs

When a top-level `before` creates a record and a nested context needs different values, update rather than recreate:

```ruby
# Top-level creates default
before do
  create(:admin_configuration, name: 'search_context', value: default_value)
end

# Nested context updates it
context 'with custom context' do
  before do
    AdminConfiguration.where(name: 'search_context').first
      .update(value: Sequel.pg_jsonb_wrap(custom_value))
    AdminConfiguration.refresh!(concurrently: false)
  end
end
```

## Testing services

Pattern for service objects:

```ruby
RSpec.describe MyService do
  subject(:result) { described_class.call(**params) }

  let(:params) { { query: query, option: option } }
  let(:query) { 'default' }
  let(:option) { true }

  describe '.call' do
    context 'when valid input' do
      it 'returns success' do
        expect(result).to be_success
      end
    end

    context 'when invalid input' do
      let(:query) { nil }

      it 'returns failure' do
        expect(result).to be_failure
      end
    end
  end
end
```
