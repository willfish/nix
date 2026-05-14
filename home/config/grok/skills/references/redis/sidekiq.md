# Sidekiq + Redis Best Practices

## Job Design

- Jobs should be **idempotent** when possible.
- Pass primitive values (IDs, small hashes) — never full ActiveRecord/Sequel objects.
- Keep jobs small and single-purpose.

## Retries and Dead Jobs

```ruby
class ImportantJob
  include Sidekiq::Job
  sidekiq_options retry: 10, queue: :critical, dead: false
end
```

Use `Sidekiq::DeadSet` monitoring for jobs that keep failing.

## Unique Jobs

Use the `sidekiq-unique-jobs` gem (or Sidekiq Enterprise) to prevent duplicate jobs:

```ruby
sidekiq_options unique: :until_executed
```

## Reliability

- Use a persistent Redis backend with proper persistence settings (`appendonly yes`).
- Monitor Redis memory and latency.
- Consider Sidekiq Pro or Enterprise for reliable queuing, batching, and metrics if you have critical background work.

## Testing

Use `Sidekiq::Testing.inline!` or `fake!` in tests. Prefer `inline!` for most integration-style tests so side effects happen synchronously.
