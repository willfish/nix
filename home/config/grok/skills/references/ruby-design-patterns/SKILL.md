---
name: ruby-design-patterns
description: >
  Practical Ruby design patterns that are actually useful in Rails + Sequel applications.
  Focuses on patterns that improve maintainability, testability, and clarity rather than academic Gang of Four implementations.
  Use when designing new features or refactoring bloated models/controllers.
---

# Ruby Design Patterns (Practical Edition)

This reference covers design patterns that provide real value in Ruby, especially in Rails applications using Sequel.

The goal is not to apply patterns for their own sake, but to solve common problems of complexity, testing difficulty, and poor separation of concerns.

## Core Recommendation

**Favor composition and small, focused objects over inheritance and fat models.**

Most "design pattern" problems in Rails come from putting too much responsibility in ActiveRecord/Sequel models or controllers.

## High-Value Patterns for This Stack

### 1. Service Objects / Interactors

Use when an action involves multiple models, external services, or complex business rules.

```ruby
class CreateOrder
  def call(customer:, items:)
    DB.transaction do
      order = Order.create(customer_id: customer.id, status: "pending")
      order.line_items_dataset.multi_insert(items)
      Inventory.reserve(items)
      order
    end
  end
end
```

**Guidelines**:
- One public method (`call` or `perform`).
- Return a Result object or the created resource.
- Keep them stateless when possible.
- Name them after the action (`CreateOrder`, `ProcessRefund`, `SendWelcomeEmail`).

### 2. Form Objects

Use for complex form handling, especially when data spans multiple models or requires special validation.

```ruby
class UserRegistrationForm
  include ActiveModel::Model
  # or use dry-validation / custom validation

  attr_accessor :email, :password, :name

  validates :email, presence: true, format: { with: URI::MailTo::EMAIL_REGEXP }

  def save
    return false unless valid?

    DB.transaction do
      user = User.create(email: email, name: name)
      user.add_credential(password: password)
      user
    end
  end
end
```

### 3. Query Objects

Encapsulate complex or reusable queries.

```ruby
class RecentPaidOrdersQuery
  def initialize(relation = Order.dataset)
    @relation = relation
  end

  def call
    @relation
      .paid
      .where { created_at > 30.days.ago }
      .order(Sequel.desc(:created_at))
  end
end
```

This works especially well with Sequel's dataset approach.

### 4. Policy Objects

Authorization logic that doesn't belong in models or controllers.

```ruby
class OrderPolicy
  def initialize(order, user)
    @order, @user = order, user
  end

  def show?
    owner? || admin?
  end

  private

  def owner? = @order.customer_id == @user.id
  def admin? = @user.admin?
end
```

### 5. Decorators / Presenters

Use for view-specific logic.

```ruby
class OrderPresenter
  def initialize(order)
    @order = order
  end

  def formatted_total
    Money.new(@order.total_cents, "USD").format
  end

  def status_badge_class
    case @order.status
    when "paid" then "bg-green-100"
    when "pending" then "bg-yellow-100"
    else "bg-gray-100"
    end
  end
end
```

### 6. Null Object Pattern

```ruby
class NullUser
  def name = "Guest"
  def admin? = false
  def active? = false
end
```

Useful for reducing nil checks in views and services.

### 7. Adapter Pattern

Useful when integrating with multiple payment providers, email services, etc.

```ruby
class PaymentGateway
  def initialize(provider)
    @adapter = case provider
               when "stripe" then StripeAdapter.new
               when "braintree" then BraintreeAdapter.new
               end
  end

  def charge(amount)
    @adapter.charge(amount)
  end
end
```

## Anti-Patterns to Avoid

- **God Objects**: Putting everything in `User` or `Order`.
- **Callbacks for business logic**: Move complex side effects into service objects.
- **Inheritance hierarchies** deeper than 1-2 levels.
- **Concerns** that just move the mess around (use them sparingly and intentionally).

## General Guidelines

- Keep models focused on persistence and associations.
- Extract behavior into small, well-named objects.
- Prefer plain Ruby objects over "Rails magic" when the logic gets complicated.
- Test the collaborators in isolation.

---

This reference will grow with more specific pattern examples tailored to Ruby + Sequel + Rails as we encounter them in real code.
