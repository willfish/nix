# Gemini And Antigravity Support

Gemini CLI and Antigravity use the same canonical harness content as Grok, Claude Code, and Codex.

## Current state

- Universal rules are deployed as `~/.gemini/GEMINI.md`, `~/.gemini/AGENTS.md`, `~/.agents/AGENTS.md`, and Antigravity-specific fallback paths.
- Shared guides are deployed under `~/.gemini/guides/`, `~/.agents/guides/`, `~/.antigravity/guides/`, and `~/.antigravitycli/guides/`.
- Shared workflow skills are deployed from `home/config/llm/skills/` into `~/.gemini/skills/` and `~/.agents/skills/`.
- The Grok process skills and reference library are also deployed into Gemini and `.agents` skill roots.

## Layout

Gemini CLI currently discovers user skills from `~/.gemini/skills/` and the `~/.agents/skills/` alias. Antigravity support is treated as the same harness plus defensive fallback paths until its CLI/IDE conventions settle.

Canonical sources:

- `home/config/llm/AGENTS.md` — portable harness rules.
- `home/config/llm/guides/` — detailed job-specific guides.
- `home/config/llm/skills/` — shared job-specific `SKILL.md` wrappers.
- `home/config/grok/skills/` — process skills and the broader reference library.

Home Manager expands those sources into each tool's expected config directory in `home/user/config.nix`.

## Goal

Switching between TUIs should be almost zero-friction for harness rules, skills, and personal/job-specific workflows. The only intended differences are each tool's native capabilities and keybindings.
