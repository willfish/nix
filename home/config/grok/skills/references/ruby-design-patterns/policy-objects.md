# Policy Objects (Authorization)

## Purpose

Extract authorization logic from models and controllers so it can be tested and reused independently.

## Example

```ruby
class OrderPolicy
  def initialize(order, user)
    @order = order
    @user = user
  end

  def show?
    owner? || admin? || support?
  end

  def update?
    owner? && @order.pending?
  end

  def cancel?
    (owner? || admin?) && @order.cancellable?
  end

  private

  def owner? = @order.customer_id == @user.id
  def admin? = @user.admin?
  def support? = @user.support?
end
```

## Usage

In controllers or services:

```ruby
policy = OrderPolicy.new(order, current_user)

unless policy.show?
  raise Forbidden
end
```

Or as a predicate:

```ruby
def show?
  OrderPolicy.new(@order, current_user).show?
end
```

## Benefits

- Clear, testable authorization rules.
- Easy to add new roles or conditions.
- Avoids scattering authorization logic across the codebase.

## Recommendations

- Keep policies focused on **one resource**.
- Use clear, intention-revealing method names (`show?`, `cancel?`, `approve?`).
- Prefer database-level constraints for critical integrity rules (policies are for application-level authorization).

## Alternatives

- **Pundit** gem — very popular in the Rails community and works fine with Sequel.
- **Action Policy** — another good option with more features.

For simple cases, plain Ruby policy objects are often sufficient and have zero dependencies.
