# Pattern Matching in Practice

Pattern matching is one of the highest-leverage features added to Ruby in recent years.

## When to Use It

- Complex conditional logic
- Destructuring API responses or job payloads
- Validating and extracting data in one step

## Good Examples

### Controller / Service Layer

```ruby
case params
in { user: { email:, password: } }
  authenticate(email, password)
in { token: }
  authenticate_with_token(token)
else
  render_unauthorized
end
```

### Job Payloads

```ruby
case job.payload
in { type: "email", to:, subject: }
  deliver_email(to, subject)
in { type: "sms", phone: }
  send_sms(phone)
end
```

### With `in` operator (Ruby 3.2+)

```ruby
users.any? { |u| u in { active: true, role: "admin" } }
```

## Tips

- Use `_` to ignore values.
- Use `*rest` for variable-length arrays.
- Combine with guards when needed:

```ruby
case user
in { age: } if age >= 18
  allow_adult_content
end
```

## When Not to Use It

- Very simple conditions (a plain `if` is clearer).
- When the destructuring becomes more complex than the original logic.

Pattern matching shines when it makes the *shape* of the data explicit. Use it for that.
