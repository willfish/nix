---
name: rspec-sequel
description: >
  Best practices for testing Ruby on Rails + Sequel applications with RSpec.
  Covers transactional testing, factory strategies, testing queries, transactions, and common patterns that differ from Active Record testing.
  Use when writing or reviewing RSpec tests in a Sequel codebase.
---

# RSpec Testing with Sequel

Testing with Sequel is generally faster and more explicit than with Active Record, but it requires different patterns in some areas.

## Core Recommendations

### 1. Use Database Transactions for Tests (When Possible)

```ruby
# spec/spec_helper.rb or rails_helper.rb
RSpec.configure do |config|
  config.around(:each) do |example|
    DB.transaction(rollback: :always) { example.run }
  end
end
```

This is the Sequel equivalent of `use_transactional_fixtures = true` and is usually the best default.

**When not to use it**:
- When testing code that uses `LISTEN`/`NOTIFY`
- When testing multiple database connections
- Some complex integration tests

### 2. Factory Strategy

Prefer **FactoryBot** with Sequel. It works very well.

```ruby
FactoryBot.define do
  factory :user do
    sequence(:email) { |n| "user#{n}@example.com" }
    name { "Test User" }
  end

  factory :order do
    association :customer, factory: :user
    status { "pending" }
    total_cents { 1999 }
  end
end
```

**Tips**:
- Use `create` when you need the record in the database.
- Use `build` when you only need the object (much faster).
- For associations, prefer explicit `association :customer` over implicit magic.

### 3. Testing Queries / Dataset Methods

Since much of your logic lives in `dataset_module`, test it at the dataset level:

```ruby
RSpec.describe Order do
  describe ".paid" do
    it "returns only paid and completed orders" do
      paid = create(:order, status: "paid")
      create(:order, status: "pending")

      expect(Order.paid.all).to contain_exactly(paid)
    end
  end
end
```

### 4. Testing Transactions

```ruby
it "rolls back on failure" do
  expect {
    DB.transaction do
      create(:order)
      raise Sequel::Rollback
    end
  }.not_to change(Order, :count)
end
```

### 5. Testing Service Objects

Service objects should be tested with real database interactions (using the transactional wrapper) but without loading the full Rails stack when possible.

```ruby
RSpec.describe CreateOrder do
  let(:customer) { create(:user) }

  it "creates an order and reserves inventory" do
    result = described_class.new(customer: customer, items: [...]).call

    expect(result).to be_success
    expect(Order.count).to eq(1)
  end
end
```

## Common Patterns

### Shared Examples for Authorization

```ruby
RSpec.shared_examples "requires ownership" do
  it "denies access to other users" do
    other_user = create(:user)
    expect(policy.show?(other_user)).to be false
  end
end
```

### Custom Matchers

```ruby
RSpec::Matchers.define :have_status do |expected|
  match do |order|
    order.reload.status == expected
  end
end
```

## Performance Tips

- Use `FactoryBot.build` + manual `save` when you don't need associations created.
- Avoid creating unnecessary records in `before` blocks.
- Use `let` instead of `let!` unless the record must exist before the example runs.
- For very slow test suites, consider `DatabaseCleaner` with truncation only for specific examples that need it.

## Sequel-Specific Testing Gotchas

- `Model#save` can fail silently if validations are not set up (use `save!` in tests when you expect success).
- Associations are lazy by default — use `.eager` or `.all` when you need to assert on associated data.
- Dataset methods return datasets, not arrays. Call `.all`, `.first`, etc. when asserting.

## Recommended Test Organization

```
spec/
├── factories/
├── models/               # Light model tests (mostly dataset methods)
├── services/             # Most of your business logic tests
├── policies/
├── queries/
└── requests/             # Integration tests (fewer, but important)
```

Focus most of your test effort on **services** and **important query behavior**, not on the models themselves.

---

This reference will be expanded with more specific RSpec + Sequel patterns (especially around testing complex queries, transactions, and performance-sensitive code).
