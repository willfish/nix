---
name: skill-evaluation
description: Skill harness evaluation and maintenance. Use for auditing skills, checking token/disclosure drift, running audit-skills, or planning skill evaluation.
disable-model-invocation: true
---

# Skill Evaluation

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Use this for local skill-harness maintenance and evaluation.

## Default Workflow

1. From the dotfiles checkout, run the static audit first:

```bash
direnv exec . nix shell .#repo-tools -c audit-skills --root "$PWD"
```

2. Treat audit findings as local maintenance work:

- Fix missing skill deployment entries or stale skill names immediately.
- Fix missing `references/*.md` links before editing content.
- Add contents sections to long references or split them by branch.
- Review trigger overlap before widening descriptions.
- Check freshness metadata when external guidance is involved.

## Completion

Before claiming harness maintenance is complete, run from the dotfiles checkout:

```bash
direnv exec . nix shell .#repo-tools -c audit-skills --root "$PWD"
nix build .#homeConfigurations.william-linux.activationPackage --dry-run
```
