# Code Reviews

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Be kind. Use simple words. Use short sentences. Write so a tired teammate can follow it.

## Tone

- Sound like a helpful colleague, not a judge.
- Say what is wrong, why it matters, and what to do.
- Keep it warm. Do not be sarcastic. Do not talk down.
- Do not pad. Do not use stiff words like "Furthermore", "Additionally", or "It's worth noting".
- Do not request changes (`--request-changes`). Use `--comment` instead. People respond to comments without needing the PR blocked.

## Language

- Prefer short common words. If a code name is needed, use the code name, then say what it does in plain words.
- One idea per sentence.
- Lead with the biggest issue.
- Use bold for the key point, then one or two short sentences.
- Use a code snippet only when words are not enough.
- Thank or praise only when it helps the author keep something good. Do not add empty praise.

## Examples

Bad:
> I noticed that the method `calculate_duty` might potentially benefit from some additional consideration regarding edge cases. It's worth noting that when the value is nil, this could lead to unexpected behaviour. Perhaps we could add a guard clause here?

Good:
> **`calculate_duty` breaks when the value is nil.** Add a guard, or check this earlier. The rest of the service looks clean.

Bad:
> This is a really great approach! I love how you've structured this. One tiny minor suggestion that you can totally ignore...

Good:
> **The query loads `measures` once per row.** Add `includes(:measures)` on line 23. Everything else looks clean.

## What to look for

1. **Correctness** - does it do what the ticket asks?
2. **Edge cases** - nil values, empty collections, concurrent access
3. **Performance** - N+1 queries, unnecessary eager loading, missing indexes
4. **Security** - mass assignment, SQL injection, auth bypass
5. **Tests** - do they cover the happy path and key edge cases?
6. **Scope** - did the PR sneak in unrelated changes?
