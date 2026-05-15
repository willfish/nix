# Gemini CLI Support (Placeholder)

This directory exists so that Gemini CLI can be given the same harness experience as Grok, Claude Code, and Codex.

## Current state

- Universal rules are deployed as `~/.gemini/GEMINI.md` (see `../AGENTS.md`).
- Job-specific guides and workflow skills are **not yet** deployed because the exact filesystem layout Gemini + Superpowers extension uses for skill discovery is not confirmed.

## When you start using Gemini

1. Install the Gemini CLI and the Superpowers extension (`gemini extensions install https://github.com/obra/superpowers`).
2. Run it once so it creates its config directories.
3. Tell me the paths it uses for:
   - Global instructions (`GEMINI.md`)
   - Skills / custom agents
   - Guides or references

We will then:
- Create thin Gemini-compatible skill wrappers under `llm/gemini/skills/` (or reuse Codex/Grok format if the extension supports it).
- Add the corresponding `home.file` entries in `home/user/config.nix`.
- Make sure the same 13 guides + the important workflow skills (`hmrc-trade-tariff-workflow`, `jira-workflow`, `will-voice`, etc.) are available with zero extra effort when you switch TUIs.

## Goal

Switching between TUIs (Grok ↔ Claude Code ↔ Codex ↔ Gemini ↔ future) should be almost zero-friction for the harness, skills, and your personal/job-specific workflows. The only difference should be the tool's native capabilities and keybindings.

This directory + the `llm/` structure is the mechanism that makes that possible.