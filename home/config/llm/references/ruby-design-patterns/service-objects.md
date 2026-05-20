# Service Objects in Ruby

## When to Use

Use a service object when an operation:
- Touches multiple models
- Has complex validation or authorization rules
- Involves external services (payments, emails, APIs)
- Should be atomic (use a transaction)

## Recommended Structure

```ruby
class CreateOrder
  def initialize(customer:, items:, payment_method:)
    @customer = customer
    @items = items
    @payment_method = payment_method
  end

  def call
    DB.transaction do
      order = create_order
      create_line_items(order)
      process_payment(order)
      send_confirmation(order)

      order
    end
  rescue PaymentFailed => e
    # handle failure
    raise
  end

  private

  def create_order
    Order.create(
      customer_id: @customer.id,
      status: "pending",
      total_cents: calculate_total
    )
  end

  # ... other private methods
end
```

## Guidelines

- One public method (`call` or `perform`).
- Return either the result or a `Result` object (success/failure).
- Keep them relatively small. If a service is doing too many things, split it.
- Name them after the **action**, not the model (`CreateOrder`, `ProcessRefund`, not `OrderCreator`).

## Testing

Service objects should be easy to test in isolation:

```ruby
RSpec.describe CreateOrder do
  it "creates an order and charges the customer" do
    result = described_class.new(...).call
    expect(result).to be_success
    expect(Order.count).to eq(1)
  end
end
```

## When Not to Use

- Simple CRUD operations (just use the model).
- Pure query logic (use a Query Object instead).
- Very small, one-off pieces of logic.

## Common Variations

- **Interactor** pattern (from the `interactor` gem) — more structured with `context` objects.
- **Command** pattern — similar idea, sometimes used with event sourcing.
- **Use Case** classes — another popular naming.

The important thing is the **separation of concerns**, not the specific naming or gem.
