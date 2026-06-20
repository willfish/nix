---
name: skill-router
description: Explicit skill catalog. Use when unsure which coding, review, Nix, document, or browser workflow applies, or when explaining available skills.
disable-model-invocation: true
---

# Skill router

Before drafting prose, read `~/.agents/guides/documentation-relevance.md`.

Use this explicit entry point to choose a workflow. Discover installed skills
through `skill_catalog`, then read the selected skill before acting. Private
workflows may be supplied by the local overlay; do not assume they are present.

- Bugs or failing tests: `systematic-debugging`.
- Significant implementation plans: `writing-plans`.
- Architecture, research or high-cost decisions: `chain-of-verification`.
- Code review: `code-review-workflow`; publishing review comments additionally
  requires `github-pr-review` and its authorization gates.
- PRs, Git conventions or Slack PR drafts: `pull-request-workflow`.
- Ruby and Rails tests: `rspec-testing`.
- Nix, Home Manager, direnv or local machine context: `local-dev-environment`.
- Diagrams: `diagramming`; terminal recordings: `terminal-demos`.
- PDFs: `latex-pdfs`; author voice: `will-voice`.
- Dependencies or publishing: `javascript-supply-chain-security`.
- Skill maintenance: `create-skill` and `skill-evaluation`.
- Completion claims, commits or pushes: `verification-before-completion`.

For authentication, accounting, browser control or external publication, discover
and load the applicable domain workflow. Catalogue matches do not authorize
operations. Preserve manual-only triggers and approval gates. If two skills
apply, load the domain workflow before the narrower mechanics skill.
