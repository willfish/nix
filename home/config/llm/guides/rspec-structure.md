# RSpec Structure

Source: https://www.betterspecs.org/
Checked: 2026-06-20
Update trigger: RSpec major release, Better Specs guidance change, recurring review feedback, or quarterly review.

Use this for RSpec style, naming, example shape, contexts, expectations, and
shared examples.

## Contents

- Describe Methods
- Use Contexts
- Keep Descriptions Short
- Single Expectation Per Test
- Test All Cases
- Use Expect Syntax
- Use Subject And Let
- Readable Matchers
- Naming Conventions
- Shared Examples

## Describe Methods

Use `.` for class methods and `#` for instance methods:

```ruby
# Good
describe '.authenticate' do
describe '#admin?' do

# Bad
describe 'the authenticate method for User' do
```

## Use Contexts

Start contexts with `when`, `with`, or `without`:

```ruby
context 'when logged in' do
context 'with valid params' do
context 'without authentication' do
```

## Keep Descriptions Short

Keep example descriptions under 40 characters. Split with contexts if needed:

```ruby
# Good
it 'returns the user' do

# Bad
it 'returns the single result with strong confidence' do

# Better
context 'with a single result' do
  it 'returns strong confidence' do
```

## Single Expectation Per Test

One behavior per test. Multiple expectations usually mean the example is
covering multiple behaviors.

Use `aggregate_failures` when checking related attributes from the same result:

```ruby
it 'returns the user attributes', :aggregate_failures do
  expect(result.name).to eq('Test')
  expect(result.email).to eq('test@example.com')
end
```

Avoid invoking the subject and then checking a mock in the same example:

```ruby
it 'calls the AI client' do
  result
  expect(OpenaiClient).to have_received(:call)
end
```

For slower request, database, or end-to-end style specs, grouping closely
related assertions can be reasonable when repeating setup would make the suite
materially slower.

## Test All Cases

Cover happy path, edge cases, and errors. Include missing records, unauthorized
access, invalid input, ownership boundaries, and empty result sets:

```ruby
describe '#destroy' do
  context 'when resource exists' do
  context 'when resource is missing' do
  context 'when user lacks permission' do
end
```

## Use Expect Syntax

```ruby
expect(actual).to eq(expected)
is_expected.to be_valid
```

Avoid:

```ruby
actual.should eq(expected)
```

## Use Subject And Let

```ruby
subject(:user) { described_class.new(attrs) }

let(:attrs) { { name: 'Test' } }
let!(:eager_loaded) { create(:thing) }
```

Use `let!` only when the record must exist before the subject runs, for example
when testing a query or scope. Otherwise prefer lazy `let`.

## Readable Matchers

Use RSpec's expressive matchers instead of manual boolean checks:

```ruby
expect { model.save! }.to raise_error(Sequel::ValidationFailed)
expect(response).to have_http_status(:not_found)
expect(payload).to include('error')
```

Avoid:

```ruby
expect(model.valid?).to eq(false)
```

## Naming Conventions

Avoid `should` in descriptions:

```ruby
it 'returns nil' do
it 'raises an error' do
```

## Shared Examples

Place repeated test patterns in `spec/support/shared_examples/`:

```ruby
RSpec.shared_examples 'does not call the AI client' do
  it 'does not call the AI client' do
    allow(OpenaiClient).to receive(:call)
    subject
    expect(OpenaiClient).not_to have_received(:call)
  end
end

context 'when query is blank' do
  let(:query) { '' }

  it_behaves_like 'does not call the AI client'
end
```

Common candidates:

- API response format checks.
- Authentication and authorization behavior.
- Pagination behavior.
- Error handling patterns.
