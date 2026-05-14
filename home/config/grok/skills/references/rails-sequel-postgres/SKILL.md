---
name: rails-sequel-postgres
description: >
  Best practices for Ruby on Rails applications using Sequel instead of ActiveRecord, with PostgreSQL as the database.
  Covers dataset vs model philosophy, transactions, query patterns, Postgres-specific features (JSONB, arrays, full-text, etc.), performance, migrations, and common patterns when coming from ActiveRecord.
  Use when working on Rails + Sequel + Postgres code, especially for architecture, performance, or complex queries.
---

# Rails + Sequel + Postgres Best Practices

This reference collects high-signal patterns for building Rails applications with **Sequel** (instead of Active Record) on **PostgreSQL**.

Sequel is a database toolkit first and an ORM second. It gives you much more direct and powerful access to SQL and Postgres features than Active Record, at the cost of some Rails magic.

## Core Philosophy

### Datasets > Models for most queries

- Use `DB[:table]` or `Model.dataset` for reads, reports, bulk operations, and complex queries.
- Use `Sequel::Model` when you need associations, validations, callbacks, or object identity.
- Prefer plain hashes/arrays over model instances when you don't need behavior.

**Rule of thumb**: If you're only reading data or doing bulk writes, stay on the dataset level.

### Embrace the database

Sequel shines when you let Postgres do what it does well (constraints, JSONB, full-text search, window functions, CTEs, etc.) instead of trying to hide it behind the ORM.

## Recommended Project Structure

```ruby
# config/initializers/sequel.rb
DB = Sequel.connect(ENV.fetch('DATABASE_URL')) do |c|
  c.extension :pg_json, :pg_json_ops, :pg_array, :pg_auto_parameterize
  c.extension :connection_validator
  c.pool_timeout = 5
end

require 'sequel_pg' if ENV['RACK_ENV'] == 'production'
```

Put reusable query logic in `dataset_module` blocks on your models:

```ruby
class Post < Sequel::Model
  dataset_module do
    select :summary, :id, :title, :published_at
    order :recent, Sequel.desc(:published_at)

    def published
      where(published: true)
    end

    def by_author(author_id)
      published.where(author_id: author_id)
    end
  end
end
```

## Key Patterns

### Transactions

```ruby
DB.transaction do
  # ...
  raise Sequel::Rollback if something_bad
end
```

- Keep transactions short.
- Use `savepoint: true` for nested transactions when needed.
- Prefer database constraints over application-level checks inside transactions.

### Eager Loading (Avoid N+1)

```ruby
# Good
Post.eager(:author, :comments).all

# Even better for complex graphs
Post.eager_graph(:author, comments: :user).all
```

Use the `tactical_eager_loading` plugin for automatic eager loading in many cases.

### JSONB in Postgres

Load the extensions, then use the powerful DSL:

```ruby
# Query
DB[:products].where(Sequel[:data].get_text('category') => 'electronics')

# Update / merge
DB[:products]
  .where(id: id)
  .update(Sequel[:data] => Sequel[:data].concat({ price: 99.99 }))
```

Always add GIN indexes on JSONB columns you query heavily.

### ON CONFLICT (Upsert)

```ruby
DB[:inventory]
  .insert_conflict(
    target: [:product_id, :warehouse_id],
    update: { quantity: Sequel[:excluded][:quantity] }
  )
  .insert(product_id: 1, warehouse_id: 2, quantity: 50)
```

### Prepared Statements for Hot Paths

```ruby
find_by_id = DB[:users].where(id: :$id).prepare(:first, :find_user_by_id)

find_by_id.call(id: 123)
```

Or use the `:prepared_statements` plugin on models.

## Common Active Record → Sequel Translations

| Active Record                  | Sequel                                      |
|--------------------------------|---------------------------------------------|
| `User.where(active: true)`     | `DB[:users].where(active: true)`            |
| `User.find_by(email: e)`       | `DB[:users].where(email: e).first`          |
| `user.posts`                   | `user.posts_dataset` or eager loading       |
| `Post.includes(:author)`       | `Post.eager(:author)`                       |
| `Post.transaction { ... }`     | `DB.transaction { ... }`                    |
| `User.update_all(active: false)` | `DB[:users].update(active: false)`        |

## Performance Recommendations

- Always install `sequel_pg` in production.
- Use `pg_auto_parameterize` extension.
- Add indexes on foreign keys + any column you filter or join on frequently.
- Use `DB[:table].explain` and `EXPLAIN ANALYZE` early.
- Prefer `multi_insert` / `import` for bulk inserts.
- Use cursors (`.use_cursor`) for very large result sets.

## Migrations

Sequel migrations are excellent. Use:

- `add_index :table, :column, concurrently: true` (in separate migration)
- `create_table` with proper constraints
- Raw SQL when the DSL gets in the way (perfectly acceptable)

## Security

Follow the official security guide. Key points:
- Never interpolate user input into SQL.
- Use `set_fields` instead of mass assignment from params.
- Always scope `update` and `delete` operations with `where`.

---

**Next steps in this reference** (will be expanded):

- Detailed query patterns
- Advanced Postgres features (JSONB, full-text, partitioning)
- Association design
- Testing strategies with Sequel
- Common pitfalls when coming from Active Record

See the files in this directory for deeper dives into each area.
