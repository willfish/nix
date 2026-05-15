# Code Reviews

## Tone

- Sound like a human, not an AI. Write the way a colleague would in a Slack DM.
- Keep explanations short and simple. Don't over-explain.
- Avoid stiff or formal phrasing - no "Furthermore", "Additionally", "It's worth noting", etc.
- Don't request changes (`--request-changes`). Use `--comment` instead. People respond to comments without needing the PR blocked.

## Structure

- Lead with the biggest issue, not a preamble.
- Use bold for the key point of each item, then a sentence or two of context.
- Code snippets only when they make the point clearer than words.
- End on something positive if the approach is generally sound.

## Examples

Bad:
> I noticed that the method `calculate_duty` might potentially benefit from some additional consideration regarding edge cases. It's worth noting that when the value is nil, this could lead to unexpected behaviour. Perhaps we could add a guard clause here?

Good:
> **`calculate_duty` blows up on nil values** - needs a guard clause or validation upstream. The rest of the service looks solid though, nice clean extraction from the controller.

Bad:
> This is a really great approach! I love how you've structured this. One tiny minor suggestion that you can totally ignore...

Good:
> **The query N+1s on `measures`** - `includes(:measures)` on line 23 would fix it. Everything else looks clean.

## What to look for

1. **Correctness** - does it do what the ticket asks?
2. **Edge cases** - nil values, empty collections, concurrent access
3. **Performance** - N+1 queries, unnecessary eager loading, missing indexes
4. **Security** - mass assignment, SQL injection, auth bypass
5. **Tests** - do they cover the happy path and key edge cases?
6. **Scope** - did the PR sneak in unrelated changes?
