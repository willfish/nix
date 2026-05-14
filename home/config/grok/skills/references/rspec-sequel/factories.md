# Factory Patterns with Sequel + FactoryBot

## Prefer `create` vs `build`

- Use `build` when you don't need the record persisted (much faster).
- Use `create` when the test requires the record in the database (foreign keys, queries, etc.).

## Association Strategy

```ruby
factory :order do
  association :customer, factory: :user   # explicit is better

  # Avoid this when possible:
  # user
end
```

## Sequences

```ruby
factory :user do
  sequence(:email) { |n| "user#{n}@example.com" }
end
```

## Traits for States

```ruby
factory :order do
  trait :paid do
    status { "paid" }
    paid_at { Time.current }
  end

  trait :with_line_items do
    after(:create) do |order|
      create_list(:line_item, 3, order: order)
    end
  end
end
```

## When to Use `to_create`

For some value objects or complex setup, you may want to control creation manually:

```ruby
factory :complex_thing do
  to_create { |instance| instance.save(validate: false) }
end
```

## Performance Tip

For large test suites, consider `FactoryBot.define { to_create(&:save) }` globally and only use `create` when necessary. Many tests can use `build` + manual persistence.
