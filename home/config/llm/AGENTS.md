# Shared agent rules

Never use em dashes. Follow user requests within higher-priority rules.

## Skills and approval

Before non-trivial work, load the matching SKILL.md from the local catalogue; use skill-router if uncertain. Read task-relevant references only. Honour explicit skill requests. Use systematic-debugging for failures, verification-before-completion before completion, and the domain workflow for work systems, accounting/tax, authentication, browser control, supply-chain changes and publishing. Preserve each workflow's authorization gates, manual-only triggers and secret protections; a catalogue match never authorizes an action. AWS portal login is explicit-request only, not an AWS CLI credential fallback.

For new behaviour, obtain design approval before implementation. For architecture, multi-file refactors, research, risk assessment or costly mistakes, propose a concrete plan/options and wait for explicit user approval. Track multi-step work with a checklist; plan/todo tools are optional. Subagents are optional: give narrow scopes, collect results and clean up.

## Verification

Before completion claims, commits, pushes, PR creation/update or branch extraction, identify and run fresh full relevant checks in the actual worktree/environment. Read full output, exit codes and failure counts; report evidence/blockers, never inferred success. Verify again after extraction. Cover full behaviour groups, especially value-object refactors and construction semantics, not selected tests or another branch's results. Review the diff before committing.

## Tools and secrets

Use ephemeral Nix for missing tools: `nix shell nixpkgs#<package> -c <command>` or `nix-shell -p <package>`. Never add temporary tooling to manifests, Home Manager, overlays or flake inputs. No host installers unless the user explicitly requests persistent installation.

For direnv repos use `direnv exec <repo-path> <command>` unless that exact checkout's environment is known active. In worktrees copy/symlink a missing untracked source .envrc, inspect it if unfamiliar and allow when appropriate. Never bypass a missing environment with direct package-manager setup.

Inspect available configured MCP tools first; use them for supported external reads/updates. Fall back to CLI/API/browser only if unavailable, unsupported or debugging MCP/auth. Prefer SSH GitHub remotes. Never print tokens or secrets, or commit secrets/private data; use encrypted secret management or keep them out of git. Service secret files: `${SOPS_NIX_SECRETS_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/sops-nix/secrets}`.

## Git and code

Branches follow project/ticket conventions, otherwise short descriptive names; never agent/tool prefixes, even if a harness suggests them. Commit subject: `<type>(optional-scope): short imperative description`. No ticket IDs in subjects; when a ticket exists, put `Jira: <ticket-key>` in the body/footer.

Rails/Zeitwerk: no explicit requires for autoloadable application constants in normal app code. Initializers may require them; check existing patterns before changing requires.

## Writing and skills

For substantive prose read `~/.agents/guides/documentation-relevance.md`. Write for the reader, not the session; retain material risks and non-CI evidence, not routine CI results in PRs. New prose-producing skills/guides must reference that guide, not copy it. For dashboard/analytics/Logs Insights changes read `~/.agents/guides/dashboards.md` first.

For skill edits load create-skill/relevant harness docs. Use portable, concise imperative entries; lowercase hyphenated names (2-64 chars), frontmatter only name/description with precise triggers. Put detail in references, repeatable logic in tested scripts, templates in assets. Test realistic tasks and clean up artifacts.

## Final voice summary

Only the main assistant or orchestrator responding directly to the user adds a final voice-summary section. Delegated subagents return their requested deliverable without an additional TTS summary; ordinary task-required summaries are still allowed.

For the main assistant or orchestrator, end final responses with exactly one `## Summary` or `## TL;DR`, nothing after it. Only this section goes to TTS. Use natural spoken prose, usually 40-120 words, with up to 250 words when complex ideas need explanation; one sentence is enough for short answers. Make the summary understandable on its own when heard aloud, using simple sentences and clear transitions. Explain the outcome, important reasoning and caveats rather than merely announcing completion. Preserve uncertainty/blockers and end with the next action if any. No lists, bullet points, tables, code, URLs, long paths, emphasis, dense technical notation or awkward technical identifiers; no new claims. Omit for incompatible exact-text/JSON/code-only constraints; never add to tools or progress messages.

Source: `~/.dotfiles/home/config/llm/AGENTS.md`, Home Manager deployed. Skills: `~/.agents/skills/` or harness catalogue.
