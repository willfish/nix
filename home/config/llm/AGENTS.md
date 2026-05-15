# AGENTS.md - Universal Rules for AI Agents (Grok, Claude, Codex, Gemini, etc.)

This file is the single source of truth for portable agent harness discipline. It is deployed via Home Manager to:

- `~/.grok/AGENTS.md`
- `~/.claude/CLAUDE.md`
- `~/.codex/AGENTS.md`
- `~/.gemini/GEMINI.md`
- (and any future TUIs)

The same universal rules file is used for all of them. Job-specific guides and workflow skills are also made available where each TUI supports them.

**Precedence**: Per-repo `AGENTS.md` / `CLAUDE.md` / `GEMINI.md` files (deeper in the directory tree) override this global document. When working inside `~/.dotfiles`, the repo-local `AGENTS.md` at the root takes precedence for Nix/Home Manager specifics.

---

## Core Harness Principles

These rules establish a disciplined, high-reliability workflow across all projects and tools. **User instructions always take highest priority.**

### 1. Skills First (The Non-Negotiable Rule)

Before any significant action, clarification, or exploration on a non-trivial task:

- Check whether any relevant skill applies (`/skills` in the current TUI, or the skills directory for your tool: `~/.grok/skills/`, `~/.claude/skills/`, `~/.codex/skills/`).
- If there is even a 1% chance a skill is relevant → invoke it (read its `SKILL.md` or equivalent and follow it exactly).
- Announce clearly: "Using [skill-name] for this..."

**Red flags** (rationalizations that mean you should have checked for a skill):
- "This is just a simple question"
- "I need more context first"
- "Let me explore the code first"
- "I can do this quickly without a skill"
- "This feels productive, I'll just start"

### 2. Todo Tracking on Complexity

For any task with 3 or more meaningful steps, or any work that will take more than a few minutes:

- Immediately create a structured checklist using the TUI's todo tool (`todo_write`, or equivalent).
- Keep statuses updated in real time (`pending` → `in_progress` → `completed` / `cancelled`).
- Break work into clear, verifiable items.

This is mandatory for non-trivial work.

### 3. Plan Mode for Significant Work

When the task involves architecture, design decisions, multi-file refactors, research, risk assessment, or anything where being wrong wastes more than ~30 minutes:

- Call the plan/enter-plan mode first.
- Explore, think, and propose a concrete plan with options.
- Only exit plan mode and start executing after the user explicitly approves the plan.

### 4. Verification Before Completion (Non-Negotiable)

**Iron Law:** No completion claims, no PR creation, no "this is done", and no extraction of changes into a new branch without fresh verification evidence.

Before claiming anything is complete or ready for review — especially before:
- Creating or pushing a PR
- Extracting a refactor or subset of changes into its own branch/PR
- Moving on from a task

You **must**:

1. Identify the actual command(s) that would prove the claim (test suite, build, etc.).
2. Run the full relevant verification command(s) from the worktree.
3. Read the output, check exit codes, and count failures.
4. Only then report the result with the evidence.

**Especially strict requirements apply to:**
- Any refactor involving `Struct`/`OpenStruct` → `Data.define` (or similar value object changes)
- Changes that affect object construction semantics
- PR extraction work (pulling one concern out of a larger mixed branch)

"Relevant tests" means the full groups that exercise the changed behaviour, not just a few files. "The original branch was green" is not sufficient evidence after extraction.

Preferred tools (in order):
1. The `verification-before-completion` or `check` skill
2. Direct `run_command` of the real test/build command with full output shown
3. Fresh subagent review

Skip verification = dishonest, not efficient.

### 5. Subagents for Independent or Parallel Work

Use subagents (with clear, narrow task descriptions) when you want:
- Independent exploration or research
- Parallel work streams
- A second opinion / review without context contamination

Always collect results via the proper output tool and clean up the subagent when done.

### 6. Progressive Disclosure & Context Efficiency

- Keep `SKILL.md` bodies concise (< ~500 lines ideal).
- Frontmatter: only `name` and `description` (the description drives auto-triggering — be specific with "use when..." keywords).
- Move detailed references, schemas, examples, and long procedures to `references/`.
- Put deterministic, repeatable logic in `scripts/`.
- Use `assets/` for templates and output artifacts.
- Default assumption: the model is already smart. Only add what it cannot reasonably know or would repeatedly rediscover.

---

## Skill Authoring Guidelines

When creating or updating skills (in `~/.grok/skills/<name>/`, `~/.claude/skills/<name>/`, `~/.codex/skills/<name>/`, or project-local equivalents):

1. **Name**: lowercase, hyphens, 2–64 characters, verb-led where possible (e.g. `systematic-debugging`, `hmrc-trade-tariff-workflow`).

2. **Description (frontmatter)**: Primary trigger. Include what the skill does + the specific contexts/keywords that should cause auto-invocation. Example: "Systematic root-cause debugging workflow. Use for any bug investigation, error tracing, or when the user reports unexpected behavior."

3. **Body**: Imperative instructions. Start with a quick-start summary, then "read X when..." guidance that points to references/scripts.

4. **Recommended structure**:
   ```
   skill-name/
   ├── SKILL.md          # lean instructions + navigation + when to use
   ├── scripts/          # executable helpers (test them!)
   ├── references/       # detailed docs, examples, long procedures (TOC if >100 lines)
   └── assets/           # templates, output artifacts, diagrams
   ```

5. **Validation**: After creation, test the skill on realistic tasks. Use subagents with fresh context for forward-testing where appropriate. Clean up test artifacts.

6. **Portability**: Skills should work across sessions and (ideally) across TUIs. Avoid hard-coded absolute paths except for well-known user directories (`~/.config`, `~/Repositories`, `~/Notes`, etc.).

See also the `create-skill` skill (or equivalent) and your TUI's skills documentation for implementation details.

---

## High-Value Native Harness Skills

These core process skills are recommended for daily use regardless of project:

- `superpowers` — Agent discipline, skills-first rule, todo tracking, plan mode, verification culture
- `systematic-debugging` — Rigorous 4-phase root-cause process (Investigation → Pattern Analysis → Hypothesis → Implementation). **Always** find root cause before proposing fixes.
- `verification-before-completion` — Strict evidence gate before claiming work is done
- `writing-plans` — Detailed, bite-sized implementation planning (TDD style)

Deeper supporting material and additional domain best-practices collections live in the reference libraries under your TUI's skills directory (e.g. `~/.grok/skills/references/`, `~/.codex/skills/.../references/`).

---

## Job-Specific Workflow Guides

For HMRC trade-tariff work (Jira AI project on `transformuk.atlassian.net`, PR conventions, epic/story writing, voice, RSpec, local testing, etc.), the following guides are available in your TUI's guides/skills system:

- `jira-workflow` / `jira.md` — API v3 usage, ADF formatting, auth via `~/.env`, transitions, issue creation/update, known quirks
- `hmrc-trade-tariff-workflow` — Top-level router for trade-tariff repos, PRs, reviews, RSpec, frontend auth, Slack summaries
- `pull-request-workflow` / `prs.md` — Title format (`AI-{story}: Imperative...`), description structure, mermaid diagrams, CLI demo GIF expectations
- `code-review-workflow` / `reviews.md` — Tone, priorities (correctness, edge cases, performance, security, tests, scope)
- `will-voice` / `voice.md` — Direct, economical, cause-and-effect technical writing style (built from 27k+ Slack messages and PR history)
- `daily-notes` / `daily-notes.md` — `~/Notes/YYYY-MM-DD/today.md` population workflow
- `rspec-testing` / `rspec.md` + `testing.md` — betterspecs.org conventions + when to write tests
- `diagramming` / `diagramming.md` + `diagramming-how-to.md` — Tool selection (Mermaid, D2, SVG), GitHub Mermaid limitations, workflows, and best practices for technical diagrams in PRs and architecture work
- `local-dev-environment` — Trade-tariff service ports, direnv usage, Brave remote debugging (9222)
- `chain-of-verification` / `planning.md` — When and how to use structured planning + adversarial challenge for accuracy-critical work
- `terminal-demos` / `terminal-demos.md` — asciinema+agg or gpu-screen-recorder+ffmpeg patterns for CLI GIFs
- `latex-pdfs` / `pdfs.md` — Professional PDF generation via `nix-shell -p texlive...`

These guides are the canonical source for institutional knowledge on the AI Jira project and trade-tariff stack. They are deployed to all supported TUIs from a single location in this repository.

**As native Grok skills**: The most frequently used workflows are also available as first-class Grok skills (discoverable via the Skill tool or `/skills`):
- `hmrc-trade-tariff-workflow`
- `jira-workflow`
- `pull-request-workflow`
- `will-voice`
- `code-review-workflow`
- `daily-notes`
- `rspec-testing`
- `diagramming`

These thin skills reference the same canonical content in `llm/guides/`.

---

**Managed by**: `~/.dotfiles/home/config/llm/AGENTS.md` via Home Manager (`home/user/config.nix`).

**Last major sync**: Codex Superpowers + job-specific HMRC trade-tariff guides consolidated into single-source `llm/` layout (2026).