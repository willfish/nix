---
name: architecture
description: >
  High-level architecture and project structure recommendations for Ruby on Rails applications using Sequel instead of Active Record.
  Covers how to organize code, where to put business logic, layering, and avoiding common Rails architecture pitfalls.
  Use when designing new applications or refactoring large codebases.
---

# Architecture for Rails + Sequel Applications

This reference provides practical architecture guidance for building maintainable applications with **Ruby on Rails + Sequel + PostgreSQL**.

## Core Principle

**Models for persistence and associations. Everything else goes somewhere else.**

The biggest architectural problem in most Rails codebases is putting too much behavior in models (or controllers). Sequel makes it easier to avoid this trap than Active Record does, because it encourages a datasets-first mindset.

## Recommended High-Level Structure

```
app/
├── models/                  # Sequel::Model classes + associations only
├── services/                # Business actions (CreateOrder, ProcessRefund, etc.)
├── queries/                 # Reusable query logic (or put in dataset_module)
├── policies/                # Authorization (OrderPolicy, UserPolicy, etc.)
├── forms/                   # Form objects for complex input handling
├── presenters/              # View-specific objects
├── jobs/                    # Background jobs
├── lib/
│   └── my_app/              # Domain logic that doesn't belong in app/
│       ├── value_objects/
│       └── services/        # Cross-cutting or complex domain services
└── controllers/
```

## Layering Guidelines

| Layer          | Responsibility                          | What belongs here                  | What does **not** belong |
|----------------|-----------------------------------------|------------------------------------|---------------------------|
| **Persistence** | Talking to the database                | Sequel::Model, dataset logic, migrations | Business rules, validations that aren't DB constraints |
| **Domain**     | Core business rules                     | Value objects, policies, domain services | Rails-specific code |
| **Application**| Use cases / orchestration               | Service objects, interactors       | Direct DB queries (usually) |
| **Interface**  | HTTP, jobs, CLI, etc.                   | Controllers, jobs, mailers         | Business logic |

## Practical Recommendations

### 1. Keep Models Thin

Your `Sequel::Model` classes should primarily contain:
- Table mapping (`set_dataset`)
- Associations
- Dataset methods (via `dataset_module`)
- Very simple instance methods that are clearly about that row

Move anything that orchestrates multiple models or external systems into a Service Object.

### 2. Use `dataset_module` Aggressively

This is one of Sequel's biggest advantages over Active Record. Put query logic close to the model where it belongs:

```ruby
class Order < Sequel::Model
  dataset_module do
    def paid = where(status: %w[paid completed])
    def for_customer(id) = paid.where(customer_id: id)
  end
end
```

### 3. Services for Complex Actions

Any action that would require multiple model saves, external calls, or complex validation should live in a Service Object (see `ruby-design-patterns/service-objects`).

### 4. Policies for Authorization

Never put authorization logic in models or controllers. Use dedicated Policy objects (see `ruby-design-patterns/policy-objects`).

### 5. Queries vs Services

- **Query Objects**: When the goal is *retrieving* data in a specific shape.
- **Service Objects**: When the goal is *doing* something (side effects, state changes).

### 6. Avoid "Concerns" for Behavior

Rails Concerns are often used as a crutch to split up fat models. This usually just moves the mess around.

Prefer composition (mixing in small, focused modules or using plain objects) over inheritance and concerns.

## When to Go Further

For very large applications, consider:

- **Bounded Contexts** (even inside a monolith)
- **Modular Rails** / **Packwerk** style boundaries
- Moving heavy domain logic out of the Rails app entirely into a `lib/` or separate gem

Most applications don't need full DDD, but they *do* need to stop putting everything in `app/models`.

## Summary

- Models = persistence + associations + dataset methods
- Services = "do this thing"
- Policies = "can this user do this?"
- Queries / Presenters / Forms = supporting roles

Keep each object small, focused, and easy to test in isolation.

This architecture plays very well with Sequel's strengths and avoids most of the pain points people experience with large Rails codebases.
