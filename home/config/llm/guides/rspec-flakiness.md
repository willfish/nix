# RSpec Flakiness And Verification

Source: https://www.betterspecs.org/
Checked: 2026-06-20
Update trigger: recurring flaky specs, formatter/preloader change, CI behavior change, or quarterly review.

Use this for focused runs, final verification, formatter output, and flaky test
work.

## Avoid Excessive Mocking

Test real behavior when practical. Mocks speed up tests but add maintenance
burden and can hide integration failures.

Good uses of stubs:

- Simulating hard-to-reach branches such as a dependency returning 404.
- Replacing external HTTP calls.
- Isolating a slow or non-deterministic dependency.

Avoid stubbing the object under test or restating its implementation through
mock expectations.

## Focused Runs And Final Verification

Run focused specs while iterating:

```sh
bundle exec rspec spec/services/my_service_spec.rb
bundle exec rspec spec/requests/api/v2/widgets_spec.rb:42
```

Before claiming a change is ready, run the full relevant group from the
worktree. For example:

```sh
bundle exec rspec spec/services spec/requests
```

If a preloader or watcher exists in the repo, use it only as a local speed aid.
The final verification command should be a direct command whose output can be
reported.

## Formatter Output

Use formatter output that makes failures easy to read. Do not change a repo's
formatter casually; follow the local `.rspec`, CI, and team conventions.
