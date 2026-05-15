# RSpec Best Practices

Based on [betterspecs.org](https://www.betterspecs.org/)

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

One behavior per test. Use `aggregate_failures` when checking related attributes:

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

## Test all cases

Cover happy path, edge cases, and errors:

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

## Avoid excessive mocking

Test real behavior when practical. Mocks speed up tests but add maintenance burden.

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

## Stub external requests

Use WebMock or VCR for HTTP calls:

```ruby
before do
  stub_request(:get, 'https://api.example.com/data')
    .to_return(body: '{"result": "ok"}')
end
```

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
