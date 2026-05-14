# AGENTS.md - Global Rules for AI Agents (Grok, Claude, Codex, etc.)

This file is the single source of truth for project rules and harness discipline. It is deployed to `~/.grok/AGENTS.md` and `~/.claude/CLAUDE.md` via Home Manager.

Grok (and compatible agents) auto-discovers it from `~/.grok/`, `~/.claude/`, and per-repo AGENTS.md / Claude.md / Agents.md files. Deeper files take precedence.

---

## NixOS / Dotfiles Essentials

- **Repo**: `~/.dotfiles`
- **Flake**: `~/.dotfiles/flake.nix`
- **Home Manager config**: `~/.dotfiles/home/user/` (packages.nix, programs.nix, config.nix, etc.)
- **Package overlays / custom**: managed via `llm-agents.nix` flake input for `grok-cli`, `codex`, `claude-code`
- **Local nixpkgs**: `~/Repositories/nixpkgs` used **only** for overlaid packages (e.g. `variety`)
- **Activation**: After changes to home-manager modules, run `home-manager switch --flake ~/.dotfiles#william` (or the appropriate host config)
- **Testing changes**: Prefer `nix build` or `home-manager build` before switching. Use `nix flake check` for basic validation.

## Browser Integration (for agents with browser tools)

Brave is configured with remote debugging: `--remote-debugging-port=9222` in `home/user/programs.nix`.

- Verify: `curl -s http://127.0.0.1:9222/json/version`
- Use `brave-devtools` MCP tools (or equivalent) for tab interaction, navigation, screenshots, etc.
- Prefer `gh` CLI for GitHub data over scraping to save tokens.
- Use `evaluate_script` for extracting page content instead of full snapshots when possible.

---

## Grok CLI Harness Best Practices (adapted from Codex Superpowers + native Grok features)

These rules establish a disciplined, high-reliability workflow. **User instructions always take highest priority.**

### Core Principles
- **Skills first**: Before any significant action or clarification, consider whether a relevant skill applies (`/skills`, project or user `~/.grok/skills/`). If in doubt (even 1%), invoke the skill. Skills provide procedural knowledge that keeps context lean via progressive disclosure.
- **Todo tracking for complexity**: For any task with 3+ steps, use the `todo_write` tool immediately to create a checklist. Update statuses as you progress. This is mandatory for non-trivial work.
- **Plan before implement**: For architectural changes, refactors, or multi-file work, call `enter_plan_mode` first to explore and propose a plan. Exit with `exit_plan_mode` only after user approval.
- **Verification before completion**: Never declare success without verification. Use `run_command` to test (build, switch dry-run, lint, unit tests), the `check` skill where applicable, or subagents for independent review. For Nix changes, at minimum run `nix flake check` and validate the specific module.
- **Subagents for parallel / independent work**: Use `spawn_subagent` (or Grok's subagent tools) for exploration, review, or isolated implementation. Provide minimal context; avoid leaking intended answers.
- **Progressive disclosure in skills & docs**: When authoring or extending skills (see `create-skill` skill and `~/.grok/skills/`):
  - Keep `SKILL.md` body concise (< ~500 lines ideal).
  - Frontmatter: only `name` and `description` (the description drives auto-triggering — be specific with keywords and "use when..." triggers).
  - Move detailed references, schemas, examples to `references/` (loaded on-demand).
  - Put deterministic, repeatable logic in `scripts/` (executed without full context load when possible).
  - Use `assets/` for templates/output artifacts.
  - Reference files from SKILL.md with clear "read X when..." guidance.
- **Context efficiency**: Challenge every token. Default assumption: the model is smart. Only add what it cannot reasonably know or would repeatedly rediscover.

### Editing Conventions (Nix / Home Manager)
- Prefer small, focused changes. Run `git diff` mentally or actually before committing.
- After editing `.nix` files under `home/user/`, test with a build: `nix build .#homeConfigurations.william.activationPackage` (adjust for your host/username).
- For programs.nix or packages.nix additions, ensure the package exists in the flake inputs or nixpkgs.
- Shell config lives in `shells.nix` + `programs.nix`. Fish is primary.
- Secrets / private data: never commit. Use `agenix` or similar if present; otherwise keep out of git.

### Workflow Preferences
1. Understand the request + explore relevant code/docs with minimal context.
2. If complex → plan mode or todo list.
3. Invoke relevant skill(s).
4. Make changes using precise edits (`search_replace` preferred over broad writes).
5. Verify (build/test/lint + `check` skill or manual run).
6. Summarize changes and any follow-up (e.g. `home-manager switch` command for the user).

### What NOT to do
- Do not run `home-manager switch` or system rebuilds without explicit user confirmation (destructive / state-changing).
- Avoid large unverified refactors.
- Do not ignore existing patterns in the flake or modules.

---

## Skill Authoring Guidelines (for `create-skill` and custom skills)

When creating or updating skills for Grok (in `~/.grok/skills/<name>/` or `<repo>/.grok/skills/<name>/`):

1. **Name**: lowercase, hyphens, 2-64 chars, verb-led where possible (e.g. `systematic-debugging`).
2. **Description (frontmatter)**: Primary trigger. Include what it does + specific contexts/keywords that should cause auto-invocation. Example: "Systematic root-cause debugging workflow. Use for any bug investigation, error tracing, or when user reports unexpected behavior."
3. **Body**: Imperative instructions. Start with quick-start, then when to read references/scripts. Use checklists, decision trees, concrete before/after examples.
4. **Structure** (recommended):
   ```
   skill-name/
   ├── SKILL.md          # lean instructions + navigation
   ├── scripts/          # executable helpers (test them!)
   ├── references/       # detailed docs (TOC if >100 lines)
   └── assets/           # output artifacts / templates
   ```
5. **Validation**: After creation, test the skill on realistic tasks. Use subagents (fresh context) for forward-testing where appropriate. Clean up test artifacts.
6. **Portability**: Skills should work across sessions. Avoid hard-coded absolute paths except for well-known user dirs (`~/.config`, `~/Repositories`).

See also the `create-skill` skill and Grok's `docs/user-guide/08-skills.md` for implementation details.

---

## References & Further Reading

- Grok user guide: `~/.grok/docs/user-guide/` (especially 08-skills.md, 11-project-rules.md, 15-subagents.md, 18-plan-mode.md)
- Superpowers harness + skill system patterns (source of many workflows here)
- Custom best practices for this stack live in `~/.grok/skills/references/` (Ruby, Rails, Sequel, Postgres, Stimulus, RSpec, etc.)

---

**Managed by**: `~/.dotfiles/home/config/grok/AGENTS.md` via Home Manager (`home/user/config.nix`).
**Last sync**: Codex Superpowers + skill-creator patterns integrated for Grok harness (2026).

## High-Value Reference Library + Native Harness Skills

**Core harness skills** (recommended for daily use):
- `superpowers` — Agent discipline, todo tracking, plan mode, verification culture
- `systematic-debugging` — Rigorous root-cause debugging process
- `verification-before-completion` — Strict evidence gate before claiming work is done
- `writing-plans` — Detailed, bite-sized implementation planning (TDD style)

**Reference material**
Deeper supporting documents and additional collections (full Superpowers references, plugin evaluation framework, React/Supabase best practices, etc.) are available in:
`~/.grok/skills/references/`

These are maintained as a high-quality reference library. The four skills listed above are the primary, Grok-native versions.
