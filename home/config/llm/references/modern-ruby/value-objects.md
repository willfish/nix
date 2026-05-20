# Value Objects in Modern Ruby

## Prefer `Data` over `Struct`

```ruby
# Old (avoid for new code)
Money = Struct.new(:amount, :currency) do
  def to_s
    "#{amount} #{currency}"
  end
end

# New (recommended)
Money = Data.define(:amount, :currency) do
  def to_s
    "#{amount} #{currency}"
  end
end
```

Benefits of `Data`:
- Immutable by default
- Better `#==` and `#hash` behavior
- `with(...)` method for easy updates
- Cleaner inspection

## Common Value Objects Worth Creating

- `Money`
- `Address`
- `Coordinates`
- `Slug`
- `Email`
- `DateRange`

## Pattern Matching with Value Objects

```ruby
case payment_method
in CreditCard[number:, expiry:]
  process_card(number, expiry)
in BankTransfer[iban:]
  process_bank(iban)
end
```

## Immutability Guidelines

- Make value objects immutable unless you have a very good reason.
- Use `freeze` on arrays/hashes you return from value objects.
- Consider using `Gem::Version` style patterns for comparable value objects.
