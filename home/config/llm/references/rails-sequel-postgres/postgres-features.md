# PostgreSQL-Specific Features with Sequel

Sequel has excellent, first-class support for modern PostgreSQL. Always load the relevant extensions.

## Recommended Extensions

```ruby
DB.extension :pg_json, :pg_json_ops, :pg_array, :pg_range, :pg_hstore, :pg_auto_parameterize
```

Also strongly recommended: `sequel_pg` gem (C extension for much faster row fetching).

## JSONB

```ruby
# Querying
DB[:products]
  .where(Sequel[:data].get_text('category') => 'books')
  .where(Sequel[:data].contains({ in_stock: true }))

# Updating / merging
DB[:products]
  .where(id: id)
  .update(Sequel[:data] => Sequel[:data].concat({ price: 29.99 }))
```

**Always** add a GIN index:

```ruby
add_index :products, :data, using: :gin, opclass: :jsonb_path_ops
```

## ON CONFLICT (Upsert)

```ruby
DB[:counters]
  .insert_conflict(
    target: :product_id,
    update: { count: Sequel[:excluded][:count] }
  )
  .insert(product_id: 42, count: 1)
```

## Arrays

```ruby
DB.extension :pg_array

DB[:posts].where(Sequel.pg_array(:tags).contains(['ruby', 'rails']))
```

## Full-Text Search

```ruby
# Simple
DB[:articles].where(Sequel.lit("to_tsvector('english', title || ' ' || body) @@ plainto_tsquery('english', ?)", query))

# Better: Use a dedicated tsvector column + GIN index
```

## RETURNING

```ruby
DB[:orders]
  .returning(:id, :number)
  .insert(status: 'pending', customer_id: 5)
```

## Listen / Notify (Real-time)

```ruby
DB.listen('order_updates') do |channel, pid, payload|
  # handle update
end
```

Note: `LISTEN` does not work inside a transaction until commit.

## Other Useful Postgres Features

- `create_function` / `create_trigger`
- `create_table` with `partition_by`
- Advisory locks: `DB.get(Sequel.function(:pg_advisory_lock, key))`
- `FOR UPDATE` / `FOR SHARE` row locking
- `COPY` for very large imports/exports

## Migration Tips

- Use `concurrently: true` for index creation in production (in its own migration).
- Use database constraints (`null: false`, `unique`, `check`) aggressively.
- Prefer `change_column_null`, `add_foreign_key`, etc. over raw SQL when possible.

Sequel makes almost all of Postgres pleasant to use from Ruby. Lean into it.
