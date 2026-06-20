# RSpec Factories And Data

Source: https://www.betterspecs.org/
Checked: 2026-06-20
Update trigger: FactoryBot/RSpec major release, local test data pattern change, or quarterly review.

Use this for FactoryBot, fixtures, and database setup in specs.

## Create Minimal Test Data

Only create records the example needs:

```ruby
# Good
let(:user) { create(:user) }

# Bad
let(:users) { create_list(:user, 50) }
```

If dozens of rows seem necessary, reconsider the design or fixture shape.

## Use Factories Over Fixtures

```ruby
# Good
let(:user) { create(:user, :admin) }

# Avoid
fixtures :users
```

For isolated unit tests, prefer plain Ruby objects or explicit value objects
over factories when persistence is not part of the behavior.

## Updating Records In Specs

When a top-level `before` creates a record and a nested context needs different
values, update rather than recreate:

```ruby
before do
  create(:admin_configuration, name: 'search_context', value: default_value)
end

context 'with custom context' do
  before do
    AdminConfiguration.where(name: 'search_context').first
      .update(value: Sequel.pg_jsonb_wrap(custom_value))
    AdminConfiguration.refresh!(concurrently: false)
  end
end
```
