---
name: redis
description: >
  Best practices for using Redis with Ruby on Rails + Sequel applications.
  Covers caching, background jobs (Sidekiq), rate limiting, real-time features, distributed locks, and connection management.
  Use when working with Redis in any capacity (caching, jobs, real-time, etc.).
---

# Redis Best Practices

Redis is an incredibly versatile tool. In a Rails + Sequel stack it is most commonly used for:

- Caching (Rails.cache)
- Background job processing (Sidekiq)
- Rate limiting / throttling
- Real-time features (Action Cable / AnyCable + pub/sub)
- Distributed locks and leader election
- Session storage (less common now)

## Connection Management

Always use a connection pool in production.

```ruby
# config/initializers/redis.rb
$redis = ConnectionPool.new(size: 5, timeout: 5) do
  Redis.new(url: ENV.fetch("REDIS_URL"))
end
```

For Sidekiq, configure the client and server pools separately.

## Caching

### Use Rails.cache with Redis store

```ruby
Rails.cache.fetch("user/#{id}/dashboard", expires_in: 5.minutes) do
  expensive_dashboard_query
end
```

### Cache keys

Include relevant version or timestamp in the key when data can change:

```ruby
"post/#{post.id}/v#{post.updated_at.to_i}"
```

### Russian Doll Caching

Combine fragment caching with `touch: true` on associations for efficient invalidation.

## Sidekiq Best Practices

- Keep job classes small and focused.
- Pass IDs, not full objects, to jobs.
- Use `sidekiq_options retry: 5, queue: :critical`
- Monitor with Sidekiq Web + Prometheus/Grafana.
- Use reliable queuing (Sidekiq Pro or similar) for critical jobs if budget allows.

## Rate Limiting

Use the `redis` gem + a simple Lua script or the `ratelimit` gem for clean rate limiting:

```ruby
limiter = Ratelimit.new("api_requests", redis: $redis)
limiter.add(user.id)

if limiter.exceeded?(user.id, threshold: 100, interval: 3600)
  raise RateLimitExceeded
end
```

## Distributed Locks

For operations that must only run once across multiple processes/servers:

```ruby
def with_lock(key, expires_in: 30.seconds)
  if $redis.set("lock:#{key}", "1", nx: true, ex: expires_in.to_i)
    begin
      yield
    ensure
      $redis.del("lock:#{key}")
    end
  end
end
```

Consider the `redlock` gem for more robust multi-node locking.

## Real-time Features

When using Action Cable or AnyCable:
- Use Redis as the pub/sub adapter (`config.action_cable.adapter = :redis`).
- Be mindful of channel subscription limits and memory usage.
- Prefer Turbo Streams over custom WebSocket logic when possible.

## Performance & Monitoring

- Use `redis-cli --bigkeys` and `redis-cli --memkeys` to find memory hogs.
- Set sensible `maxmemory` policies (usually `allkeys-lru` or `volatile-lru`).
- Monitor keyspace notifications if you rely on them.
- Use `redis-rb` connection pooling + `hiredis` driver when possible.

## Common Pitfalls

- Storing large objects in Redis (it is not a document store).
- Not setting expiration on cache keys.
- Using Redis as a primary database (it is not).
- Long-running Lua scripts blocking the event loop.

## Recommended Gems

- `sidekiq`
- `redis`
- `connection_pool`
- `ratelimit` or `redis-semaphore`
- `redlock` (if needed)

---

This reference will be expanded with more specific patterns (Sidekiq reliability, cache stampede protection, real-time presence, etc.) as they become relevant.
