# JavaScript Supply Chain Security

Use this guide for npm, Yarn, pnpm, Bun, Node package updates, lockfile review, package publishing, and JavaScript dependency security.

## Baseline Policy

- Treat package installation as code execution. Lifecycle scripts are disabled by default on William's machines.
- Prefer `npm ci`, `yarn install --immutable`, or equivalent reproducible install commands for verification.
- Do not run `npm install`, `yarn add`, `pnpm add`, or lockfile-changing commands in a repo without inspecting the resulting manifest and lockfile diff.
- Do not bypass script controls just to make an install succeed. Identify the package that needs scripts, check why, then allow it only for that operation or repo.
- Prefer exact direct dependency versions. Let lockfiles capture transitive versions.
- Prefer registry dependencies over git, tarball, local path, or remote URL dependencies. Treat non-registry sources as high risk.
- Do not store npm tokens in dotfiles, repo files, shell history, or CI logs. Prefer OIDC trusted publishing over long-lived publish tokens.

## Local Machine Defaults

Home Manager manages:

- `~/.npmrc`:
  - `ignore-scripts=true`
  - `allow-git=none` on npm versions that support it
  - `save-exact=true`
- `~/.yarnrc` for Yarn Classic:
  - `ignore-scripts true`
- `~/.yarnrc.yml` for Yarn Berry:
  - `enableScripts: false`
  - `checksumBehavior: throw`
  - `npmMinimalAgeGate: 3d`
  - `npmPublishProvenance: true`

When a package manager does not support one of these keys, prefer upgrading the toolchain over weakening the policy.

## Installing Dependencies

Before install:

1. Read `package.json` scripts and dependency changes.
2. Check the package manager and expected lockfile:
   - npm: `package-lock.json` or `npm-shrinkwrap.json`
   - Yarn Classic: `yarn.lock`
   - Yarn Berry: `yarn.lock` plus `.yarnrc.yml`
   - pnpm: `pnpm-lock.yaml`
   - Bun: `bun.lock` or `bun.lockb`
3. Prefer the repo's existing package manager. Do not introduce a second lockfile.

After install:

1. Inspect manifest and lockfile diffs.
2. Look for new lifecycle scripts, native builds, binary downloads, postinstall fetchers, or package manager changes.
3. Run the relevant verification command from the repo, not just a narrow smoke test.
4. If a dependency update changes hundreds of transitive packages, summarize the reason before accepting it.

## Temporarily Allowing Scripts

Use the smallest bypass that works:

- npm one-off: `npm install --ignore-scripts=false <package>` only after inspecting the package.
- Yarn Classic one-off: `yarn install --ignore-scripts false` only for the affected repo.
- Yarn Berry: prefer `dependenciesMeta.<package>.built: true` for known build requirements instead of enabling scripts globally.
- pnpm: prefer `pnpm approve-builds` and explicit `onlyBuiltDependencies` / `ignoredBuiltDependencies`.

Document why scripts were allowed when the change lands in a repo.

## Auditing And Risk Signals

Use audit tools as triage inputs, not automatic patches:

- npm: `npm audit`, `npm audit --omit=dev`, `npm sbom`.
- Yarn Berry: `yarn npm audit`, `yarn explain peer-requirements`.
- pnpm: `pnpm audit`.
- OpenSSF Scorecard and package metadata are useful dependency health signals.

Do not run `npm audit fix --force` or equivalent broad rewrites without reviewing semver jumps and lockfile churn.

Check high-risk packages for:

- Very recent publish age
- New maintainer or ownership transfer
- Low download count for a critical package
- Obfuscated/minified source in packages that should be readable
- Install scripts that download executables
- Repository mismatch between package metadata and source
- Missing provenance for newly published packages where provenance is expected

## Publishing Packages

Prefer:

- npm trusted publishing / OIDC from GitHub Actions.
- Provenance (`npm publish --provenance` or Yarn's `npmPublishProvenance`).
- 2FA for accounts with publish rights.
- Staged or dry-run publishing before public release.
- Minimal package contents verified by `npm pack --dry-run`.

Avoid:

- Long-lived npm automation tokens when trusted publishing is available.
- Publishing from a laptop with broad account tokens.
- Publishing packages that include secrets, test fixtures with credentials, local config, or unreviewed build artifacts.

Before publishing:

1. Run tests and build.
2. Run `npm pack --dry-run`.
3. Inspect `package.json` `files`, `exports`, `bin`, and lifecycle scripts.
4. Confirm provenance or trusted publishing is configured.

## Agent Workflow

When an agent changes JavaScript dependencies:

1. Invoke the JavaScript supply chain security skill.
2. Identify package manager, lockfile, Node version, and install command.
3. Inspect package manifests before running install commands.
4. Use exact, scoped commands.
5. Inspect resulting diffs before claiming the work is done.
6. Run the repo's verification command.
7. Report script bypasses, audit findings, and any unresolved risk.

## References

- npm trusted publishing: https://docs.npmjs.com/trusted-publishers/
- npm install configuration: https://docs.npmjs.com/cli/v11/commands/npm-install/
- npm package provenance: https://docs.npmjs.com/generating-provenance-statements
- npm staged publishing: https://docs.npmjs.com/cli/v11/commands/npm-stage/
- Yarn configuration: https://yarnpkg.com/configuration/yarnrc
- pnpm build approval: https://pnpm.io/cli/approve-builds
- OpenSSF Scorecard: https://github.com/ossf/scorecard
