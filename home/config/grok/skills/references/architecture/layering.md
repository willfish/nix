# Layering and Responsibility Guidelines

## The Problem with "Fat Models"

Putting business logic in Sequel/ActiveRecord models leads to:
- Hard-to-test code (requires database)
- God objects
- Difficult to reuse logic outside the web request cycle
- Tight coupling to the persistence layer

## Recommended Layering

### 1. Persistence Layer (Models + Datasets)

**Only** responsible for:
- Mapping objects to database rows
- Associations
- Reusable query scopes (via `dataset_module`)
- Basic database-level constraints

**Do not put here**:
- Complex business rules
- Orchestration across multiple models
- External service calls

### 2. Domain Layer (Value Objects, Policies, Domain Services)

Pure Ruby. No knowledge of Rails or the web.

This is where your actual business rules live.

### 3. Application Layer (Service Objects / Use Cases)

Orchestrates domain objects and persistence to accomplish a specific goal.

Example: `CreateOrder`, `ProcessRefund`, `SendMonthlyReport`.

### 4. Interface Layer (Controllers, Jobs, Mailers, CLI)

Only responsible for:
- Receiving input (HTTP params, job payload, CLI args)
- Calling the right application service
- Presenting the result (rendering views, returning JSON, etc.)

Controllers should be very thin.

## Practical Example

**Bad** (everything in the model):

```ruby
class Order < Sequel::Model
  def process_payment!
    # talks to Stripe
    # updates multiple tables
    # sends email
    # ...
  end
end
```

**Better** (layered):

```ruby
# Interface
class OrdersController < ApplicationController
  def create
    result = CreateOrder.new(current_user, params).call
    # handle result
  end
end

# Application
class CreateOrder
  def call
    # orchestrates
  end
end

# Domain
class OrderPolicy; end
class Money; end
```

## Key Rule

**The further down the stack (closer to the database), the dumber the objects should be.**

Business logic should live as high as reasonably possible (but not in controllers).
