---
name: modern-ruby
description: >
  Modern Ruby best practices (Ruby 3.2–3.4+). Covers language features that improve safety, performance, and expressiveness in real applications, especially in Rails + Sequel environments.
  Use when writing new Ruby code or refactoring older patterns.
---

# Modern Ruby Best Practices (2025–2026)

This reference focuses on practical, high-value features and patterns from recent Ruby versions that are worth adopting in production codebases.

## Frozen String Literals

**Default behavior is changing.** In Ruby 3.4+, string literals are frozen by default in many contexts.

**Recommendation**:
- Explicitly use `frozen_string_literal: true` at the top of files for now.
- Use `+` or `.dup` when you need a mutable string.
- Prefer `String#frozen?` checks in hot paths if needed.

```ruby
# frozen_string_literal: true

name = "hello"          # frozen
name += " world"        # creates new string
```

## Pattern Matching

One of the biggest quality-of-life improvements.

### Basic usage

```ruby
case user
in { role: "admin", active: true }
  grant_full_access
in { role: "editor", verified: true }
  grant_edit_access
else
  deny_access
end
```

### With arrays

```ruby
case response
in [200, { "data" => data }]
  process(data)
in [400..499, error]
  handle_client_error(error)
end
```

### Find pattern (Ruby 3.2+)

```ruby
users.find { |u| u in { active: true, role: "admin" } }
```

**Best practice**: Use pattern matching for control flow and data extraction instead of multiple `if` statements or `dig`.

## Data Class (Immutable Value Objects)

Ruby 3.2 introduced `Data` — a lightweight, immutable alternative to `Struct`.

```ruby
User = Data.define(:id, :name, :email)

u = User.new(1, "Alice", "alice@example.com")
u.with(email: "new@example.com")   # returns new instance
```

**When to use**:
- DTOs
- Value objects (Money, Address, Coordinates, etc.)
- Anywhere you want cheap, hashable, comparable objects without boilerplate.

Prefer `Data` over `Struct` for new code.

## Endless Methods (def foo = ...)

```ruby
def full_name = "#{first_name} #{last_name}"

def admin? = role == "admin"
```

Good for simple one-liners. Don't overuse — readability still matters.

## Hash Improvements

```ruby
params.except(:password, :password_confirmation)
params.slice(:name, :email)

# Ruby 3.4+ has even more ergonomic methods
```

## `it` Block Parameter (Ruby 3.4+)

```ruby
[1, 2, 3].map { it * 2 }           # => [2, 4, 6]
users.select { it.active? }
```

This is a nice quality-of-life improvement. Use it for short blocks where the meaning is obvious.

## Error Handling Improvements

- Better `NoMethodError` messages (shows available methods in some cases).
- `ErrorHighlight` is now built-in and excellent.
- Use `Exception#detailed_message` when logging.

## Performance & JIT

- YJIT is mature and usually worth enabling in production (`--yjit`).
- Measure before and after — gains vary by workload.
- Use `RubyVM::YJIT.enabled?` if you need to conditional behavior.

## Other Notable Mentions

- `Module#const_added` hook
- `Warning` categories (`:deprecated`, `:experimental`)
- `Prism` as the new parser (more reliable AST)
- `it` as a method in blocks (Ruby 3.4)

## Recommendations by Area

**For new code**:
- Default to `Data` for value objects.
- Use pattern matching for complex conditionals.
- Embrace frozen strings explicitly.
- Use endless methods sparingly for obvious cases.

**For refactoring older Rails code**:
- Replace many uses of `Struct` with `Data`.
- Convert complex `if/else` chains to pattern matching.
- Add `frozen_string_literal: true` file-by-file.

**For performance**:
- Enable YJIT.
- Profile before optimizing.
- `sequel_pg` + good query patterns usually give bigger wins than micro-optimizations in Ruby itself.

---

This reference will be expanded with more specific modern Ruby patterns as they prove valuable in real codebases.
