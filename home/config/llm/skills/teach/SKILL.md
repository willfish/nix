---
name: teach
description: >
  Teach the user a new skill or concept over multiple sessions using a dedicated stateful workspace.
  Use for deep, progressive learning: "teach me how X works", "help me learn Y with lessons and quizzes", "I want to understand Z properly over time", "create a learning workspace for Nix flakes / Home Manager / fish / [any topic]".
  Creates MISSION.md, learning records (like ADRs), beautiful self-contained HTML lessons, reference docs, curated resources, and a glossary. Grounds teaching in the user's real goals and zone of proximal development.
---

The user has asked you to teach them something. This is a stateful request — they intend to learn the topic over multiple sessions, possibly across days or weeks.

## Teaching Workspace

Treat a dedicated directory as the teaching workspace (the user will usually create e.g. `learning-nix-flakes/`, `learning-fish/`, or `learning-home-manager/`). The state of their learning lives in files in this directory:

- `MISSION.md`: Captures the *reason* the user is interested in the topic. Grounds every teaching decision. Use the exact format in [references/MISSION-FORMAT.md](./references/MISSION-FORMAT.md).
- `RESOURCES.md`: Curated high-trust resources (knowledge sources + communities for wisdom). Use the format in [references/RESOURCES-FORMAT.md](./references/RESOURCES-FORMAT.md).
- `./learning-records/*.md`: Numbered records (`0001-*.md`, `0002-*.md`, ...) of demonstrated understanding, prior knowledge, corrected misconceptions, and mission shifts. These are the primary signal for calculating the zone of proximal development. Use the format in [references/LEARNING-RECORD-FORMAT.md](./references/LEARNING-RECORD-FORMAT.md).
- `./reference/*.html`: Durable reference materials — cheat sheets, glossaries, syntax, algorithms, etc. These are the compressed, printable units of knowledge. They should be beautiful and quick to consult.
- `./lessons/*.html`: The primary teaching units. Short, self-contained, beautiful HTML files (`0001-<slug>.html` etc.) that deliver one tight win tied to the mission, with interactive elements (quizzes, steps) and links to references + primary sources.
- `NOTES.md`: Scratchpad for user preferences, working notes, and teaching style adjustments.

**Grok-specific workspace handling**: Ask the user for (or confirm) the target directory. Use `run_terminal_command` (or direct file tools) to `mkdir -p` subdirectories as needed. Use the `write` or `search_replace` tools to create and update the .md and .html artifacts in the workspace. When the user is working inside this repo or on dotfiles topics, consider also loading `local-dev-environment` for machine-specific context.

## Philosophy

Deep learning requires three things:

- **Knowledge** — drawn from high-quality, high-trust external resources. Never rely primarily on your parametric knowledge for the subject matter.
- **Skills** — acquired through tight, relevant interactive lessons with feedback loops.
- **Wisdom** — comes from real-world interaction with other learners and practitioners (communities).

Before `RESOURCES.md` is populated, prioritize finding and recording excellent sources. Some topics are more knowledge-heavy (e.g. language internals); others are more skills-heavy (e.g. a physical or procedural practice).

## The Mission

Every lesson and decision must connect to the mission — the concrete reason the user wants this knowledge/skill.

If `MISSION.md` does not exist or the mission is unclear, your first action is to interview the user:
- Why do they want to learn this?
- What does success look like in observable terms?
- Current level / constraints / out-of-scope adjacent topics?

Update `MISSION.md` (and add a learning record) when the mission evolves. Confirm changes with the user.

A weak or vague mission produces abstract, ungrounded lessons.

## Zone of Proximal Development (ZPD)

Always teach the next thing that is challenging *just enough* given what the user has already demonstrated.

Use the `learning-records/` to establish:
- What the user already knows at what depth.
- Recent insights and corrections.
- The current frontier.

If the user names a specific sub-topic, still cross-check against their records and mission before deciding the exact scope of the next lesson.

## Lessons (Primary Output)

Lessons are short, self-contained HTML files written to `./lessons/`.

Rules for a good lesson:
- One tightly scoped thing that delivers a single tangible win.
- Directly tied to the mission and the user's current ZPD.
- Beautiful, clean typography and layout (the user will re-read and print these).
- Includes a strong primary source recommendation (the best high-trust resource for this slice).
- Contains interactive practice with tight feedback (quizzes, step-by-step real-world actions, light in-browser tasks). Use vanilla HTML/JS so the file is standalone.
- Links (via anchors or relative paths) to relevant reference documents and earlier lessons.
- Reminds the user they can ask the agent (you) followup questions.
- For quizzes: avoid obvious tells (e.g. "the longest answer is always correct").

After writing a lesson, if practical, open it for the user:
- On this Linux/fish environment, try `xdg-open lessons/0001-foo.html` via the `run_terminal_command` tool.
- Otherwise, clearly state the absolute or relative path and how to open it (browser, etc.).

Number lessons sequentially. Update learning records after the user engages with a lesson and demonstrates (or corrects) understanding.

## Acquiring Knowledge & Skills

**Knowledge** must be sourced. Record sources in `RESOURCES.md` with annotations (what it covers and when to use it). Cite them in lessons.

**Skills** are taught via interactive feedback loops inside lessons:
- Quizzes with immediate feedback.
- Guided real-world steps the user performs outside the lesson, then reports back.
- For code/tool topics: concrete exercises the user can run in their actual environment (leverage `local-dev-environment` when relevant).

## Acquiring Wisdom

When the user needs real-world judgment or community feedback, do not pretend to be sufficient. Help identify high-signal communities (subreddits, forums, Discord, local groups, etc.) and record them in the Wisdom section of `RESOURCES.md`.

Respect explicit preferences to avoid communities.

## Reference Documents

While teaching, also produce `reference/*.html` artifacts. These are the long-term value:
- Glossaries (build a canonical set of terms with tight definitions and "avoid" notes).
- Cheat sheets, syntax tables, decision flows, pose sequences, etc.
- Designed for quick reference and printing.

Lessons are ephemeral (you do them then move on). References are the durable compressed knowledge. Once a glossary exists, use its terminology consistently in all future lessons and records.

See [references/GLOSSARY-FORMAT.md](./references/GLOSSARY-FORMAT.md) for the glossary contract (the source of truth for terminology in this workspace; often rendered or mirrored into `reference/glossary.html` or similar).

## NOTES.md

Capture user preferences about *how* they want to be taught ("I like concrete examples first", "hate multiple choice", "prefer I do the typing", "always include a failing example", etc.). Refer to these when designing future lessons.

## Grok / This Dotfiles Harness Notes

- **Invocation**: Say "teach me how ...", "create lessons for ...", "/teach <topic>", or describe the learning goal. The description frontmatter makes this trigger naturally.
- **Workspace creation**: The user (or you via tools) creates a fresh directory per major topic. One mission per workspace. Unrelated topics get separate workspaces.
- **Composition**: When the topic touches Nix, Home Manager, shells, editors, or this machine, also consult the `local-dev-environment` skill. For diagrams or visuals inside lessons, the `diagramming` skill provides guidance and can generate Mermaid/D2/etc. that you embed.
- **File operations**: Use the available file tools (`write`, `search_replace`, `read_file`) and `run_terminal_command` (for mkdir, xdg-open, ls of records, etc.) to manage the workspace state exactly as a human tutor would update a student's notebook and handouts.
- **Multi-TUI**: Because this skill lives in the dotfiles source (`home/config/llm/skills/teach/`), Home Manager deploys it to `~/.grok/skills/`, `~/.claude/skills/`, etc. The workspace files themselves are portable.
- **Quality bar**: Follow the formats strictly. They exist so outputs stay consistent and reviewable even when switching models or TUIs.

Start every new teaching engagement by ensuring `MISSION.md` exists and is concrete, then curating `RESOURCES.md` before generating the first lesson.
