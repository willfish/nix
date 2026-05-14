# Query Patterns with Sequel (Datasets First)

## Philosophy

Write reusable, cacheable, and composable query logic using `dataset_module`.

## Recommended Structure

```ruby
class Order < Sequel::Model
  dataset_module do
    # Static parts first (good for caching)
    select :summary, :id, :number, :total_cents, :status, :created_at
    order :recent, Sequel.desc(:created_at)

    def paid
      where(status: %w[paid completed])
    end

    def for_customer(customer_id)
      paid.where(customer_id: customer_id)
    end

    def in_last(days)
      where { created_at > Date.today - days }
    end
  end
end
```

Usage:
```ruby
Order.for_customer(123).in_last(30).recent.all
```

## Key Techniques

### Use `where` last when possible

This allows Sequel to cache optimized loaders.

### Eager Loading

```ruby
# One query per association
Order.eager(:customer, :line_items).all

# Single query with joins (good when you need data from joined tables)
Order.eager_graph(:customer, line_items: :product).all
```

### Virtual Rows for Complex Conditions

```ruby
# Safe, database-specific expressions
DB[:products].where { price > 100 & stock > 0 }
```

### Raw SQL when clearer

```ruby
DB[:orders].with_sql(<<~SQL, customer_id)
  SELECT * FROM orders
  WHERE customer_id = ?
    AND status IN ('paid', 'shipped')
  ORDER BY created_at DESC
  LIMIT 50
SQL
```

### Bulk Operations

```ruby
# Fast
DB[:line_items].multi_insert(rows)

# Or
DB[:products].where(id: ids).update(active: false)
```

## Common Gotchas

- `Model.all` loads everything into memory. Use `.each`, `.use_cursor`, or pagination for large tables.
- Associations on models still go through the model layer — use datasets for heavy reporting.
- `Dataset#update` and `#delete` do **not** run model hooks.

## Performance Tips

- Use `sequel_pg` (C extension) in production.
- Add `pg_auto_parameterize` extension for automatic prepared statements.
- Use `DB[:table].explain` and `EXPLAIN ANALYZE` early and often.
- Index foreign keys + any column you frequently filter or order by.
- Consider materialized views for complex reporting queries.
