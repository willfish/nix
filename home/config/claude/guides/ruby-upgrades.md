# Ruby Version Upgrade Guide

## Check for New Ruby Versions

1. Get the current version from `.ruby-version` in the repo
2. Check the latest stable Ruby release:
   ```bash
   curl -s https://cache.ruby-lang.org/pub/ruby/index.txt | grep -E '^ruby-[0-9]+\.[0-9]+\.[0-9]+\s' | tail -1
   ```
3. Only upgrade to stable releases — never previews, rc, or dev builds
4. Only upgrade patch or minor versions unless explicitly instructed to do a major upgrade

## Files to Update

### All repos

- `.ruby-version` — canonical source of truth
- `Dockerfile` — update `ARG RUBY_VERSION=X.Y.Z`

### Backend only (`trade-tariff-backend`)

- `.tool-versions` — update the `ruby X.Y.Z` line
- `.devcontainer/Dockerfile` — update the Ruby version argument

### Files that auto-detect (no changes needed)

- `Gemfile` — reads from `.ruby-version`
- CI workflows — read from `.ruby-version`

## Upgrade Process

1. Create a new branch: `git checkout -b ruby-X.Y.Z`
2. Update all relevant files listed above
3. Run `bundle install` to update `Gemfile.lock`
4. Run the test suite to verify nothing is broken
5. Commit changes with message: `Upgrade Ruby to X.Y.Z`
6. Push and create a PR with `gh pr create`
7. Post a summary to Slack `#dev-updates`:
   ```bash
   gh api --method POST "https://slack.com/api/chat.postMessage" \
     --header "Authorization: Bearer $SLACK_BOT_TOKEN" \
     --field channel="#dev-updates" \
     --field text="Ruby upgrade PR created for <repo>: <pr-url> (X.Y.Z → X.Y.Z)"
   ```

## Constraints

- Skip the repo if already on the latest version
- Skip if a Ruby upgrade PR already exists (check with `gh pr list --search "Ruby"`)
- Do not upgrade major versions unless explicitly instructed
- If tests fail, still create the PR but mark it as draft with `gh pr create --draft`
