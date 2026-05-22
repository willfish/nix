---
name: javascript-supply-chain-security
description: Use when working with npm, Yarn, pnpm, Bun, Node dependencies, package installs, lockfile diffs, dependency upgrades, package audits, JavaScript supply chain security, lifecycle scripts, npm tokens, package publishing, trusted publishing, or provenance.
---

# JavaScript Supply Chain Security

Use this skill before changing JavaScript dependencies, running install commands, reviewing package manager lockfiles, publishing npm packages, or handling npm/Yarn/pnpm/Bun security questions.

Read `references/javascript-supply-chain-security.md` for the current policy and checklist.

Default workflow:

1. Identify the package manager, lockfile, Node version, and current install command.
2. Inspect `package.json`, package-manager config, and lockfile status before running installs.
3. Preserve existing package-manager choice; do not introduce a second lockfile.
4. Keep lifecycle scripts disabled unless the package has been inspected and the bypass is scoped.
5. Prefer exact direct dependency versions and reproducible install commands.
6. Inspect manifest and lockfile diffs after changes.
7. Run the repo's relevant verification command before completion.
8. Report any script bypass, non-registry dependency, audit finding, or unresolved supply-chain risk.
