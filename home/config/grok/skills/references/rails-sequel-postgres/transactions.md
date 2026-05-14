# Transactions with Sequel + Postgres

## Core Principles

- Keep transactions as short as possible.
- Only wrap the minimal set of statements that must be atomic.
- Let the database handle integrity via constraints whenever possible.

## Basic Usage

```ruby
DB.transaction do
  order = Order.create(...)
  order.line_items_dataset.multi_insert(items)
  Inventory.decrement_stock(order)
end
```

Use `raise Sequel::Rollback` to rollback without raising to the caller.

## Savepoints (Nested Transactions)

```ruby
DB.transaction do
  # outer work

  DB.transaction(savepoint: true) do
    # inner work that can fail independently
  end
end
```

## Isolation Levels

For complex cases (e.g. preventing lost updates):

```ruby
DB.transaction(isolation: :serializable) do
  # ...
end
```

Most applications do fine with the default (`read committed`).

## Common Patterns

### Conditional Rollback

```ruby
DB.transaction do
  result = perform_critical_operation

  unless result.success?
    raise Sequel::Rollback
  end
end
```

### With Retries (for serialization failures)

```ruby
def with_transaction_retry(max_attempts: 3)
  attempts = 0
  begin
    DB.transaction(isolation: :serializable) { yield }
  rescue Sequel::SerializationFailure
    attempts += 1
    raise if attempts >= max_attempts
    sleep(0.1 * attempts)
    retry
  end
end
```

## With Models

Models participate in the current transaction automatically:

```ruby
DB.transaction do
  user = User.create(name: "Alice")
  user.add_role(:admin)
end
```

## Anti-patterns

- Long-running transactions (especially with external calls).
- Using transactions for performance (use batching instead).
- Overusing `after_commit` / `after_rollback` hooks — they add complexity.

## Postgres-Specific Notes

- `LISTEN` / `NOTIFY` does **not** work inside a transaction until commit.
- `pg_advisory_lock` can be useful for coarse-grained locking across processes.
- Use `FOR UPDATE` / `FOR SHARE` row locking when needed:

```ruby
Account.for_update.first(id: account_id)
```

## Recommended Reading

- Official Sequel transactions documentation
- Postgres transaction isolation documentation
- "Serializable" isolation for high-contention operations (inventory, payments, etc.)
