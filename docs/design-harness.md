# Design harness

Pi design workflow, role selection and tool decisions. Research was checked
against the linked primary sources on 2026-09-27; recheck access, terms and
pricing before enabling an optional service. Vendor documentation is not evidence
that an account or this Pi client can connect.

Local editable sources are the default. One `design-workflow` skill covers
every design task. Three roles split ownership. No new global MCP, paid SaaS,
or authentication is required.

## Roles

| Role | Owns | Leaves alone |
| --- | --- | --- |
| `interface-designer` | TUI, web, native and mobile task completion | Measurement and campaign strategy |
| `dashboard-designer` | Decision views, measurement and their complete UI | Unrelated application features and campaign art |
| `communication-designer` | Decks, marketing and campaign landing-page interaction | Unrelated application journeys and ops dashboards |

Route by the primary outcome. One accountable lead owns shared tokens, semantics
and final integration; the parent assigns bounded artifacts for mixed work.
Children do not delegate. Advice and critique are read-only; implementation
changes assigned artifacts only. Publishing remains a separate authorization.

Design routing is shared guidance for all thirteen roles, not only `/design`.
General roles conditionally read `design-workflow` when their task affects a
user-facing surface and apply it within their existing permissions. They request
specialist help through the coordinator. The coordinator can handle small changes
directly or dispatch a design lead; `/design` explicitly requests delegation.
This is model-guided routing, not a hardcoded classifier or automatic team launch.
Only the three designers preload the full skill; unrelated work does not.

Reuse `tui-design`, `~/.agents/guides/dashboards.md`, `diagramming`,
`browser-automation`, and `latex-pdfs`. Do not copy those texts into the new
skill. Web-marketing taste is not a terminal or dashboard rule.

## Invoke

After Home Manager activation, start a new Pi session or use `/reload` to load
`/design` and the skill registry. The standard cloud profile supports delegation;
the isolated Qwen profile does not.

With no brief, inspect the current context and return a proposed brief. Do
not change the product.

```text
/design TUI: keyboard-first search in the existing list. No colour required. Narrow and wide layouts.
/design Web: responsive account form. Keyboard, error, empty, and long values. Inspect in visible Brave.
/design Native: desktop settings pane. Name the toolkit, states, and any runtime check you cannot run.
/design Mobile: phone list-detail for the named platform. Adapt navigation and type. Do not reuse one geometry.
/design Dashboard: error rate for this service. Name the denominator, missing versus zero, and freshness. Mark synthetic series.
/design Deck: three-slide Typst deck from this brief. Editable source, PDF, and a PNG of every page.
/design Marketing: one SVG poster. Editable text. Label hypothetical copy as hypothetical.
```

Direct dispatch also works. Pass cwd, owned files, surface,
constraints, mode and the output contract. A backend-only refactor is not a design
task.

```text
subagent({ agent: "interface-designer", task: "Implement the web form in <path>. Own only those files. Do not delegate or publish." })
```

## Output contract

Return the user and task, the selected direction, maintained editable source,
export commands, a state matrix, and what was verified versus not. Render the
real output, read it, fix defects, and re-export. A screenshot is not an
accessibility certificate or a native runtime result. PDF is not image
evidence. Do not publish.

Role default: `openai-codex/gpt-6-astra`, high thinking, with declared image
input. This is not a measured quality ranking. Pass absolute image paths for
children to read; parent attachments do not automatically follow delegation.
Local sources/screenshots can be sent to the configured cloud model. Tool
allowlists and role instructions are not a security sandbox.

## Defaults

| Surface | Source of truth | Check |
| --- | --- | --- |
| TUI | Existing app and `tui-design` | Keyboard path, narrow and wide, still usable with no colour |
| Web | The repo's framework | Visible Brave on port 9222. Do not launch a browser |
| Native | The named platform toolkit | States, navigation, semantics. Unrun checks stay unrun |
| Mobile | Same, plus safe area and platform navigation | Same honesty rule |
| Dashboard | `dashboards.md` | Define the measure before drawing it. Label synthetic data |
| Deck | Typst source, then PDF and PNG | Read every page. Edit the source and re-export |
| Marketing | Editable SVG | Text stays text. Read it at the target size |

Typst is the default for new decks because it does not need a headless
browser. Existing Beamer files stay on LaTeX. Use ephemeral Nix for missing
tools, not a new Home Manager dependency for one document.

Required GUI-editable `.pptx` uses MIT-licensed python-pptx for native text,
shapes, chart data and notes, with LibreOffice for edit/export and Poppler for
page previews. Reopen/edit/re-export; LibreOffice rendering does not establish
PowerPoint or assistive-technology compatibility. Do not copy Anthropic's
restricted `pptx` skill. Typst source editability is not PPTX editability.

Use editable SVG and librsvg for graphics; Inkscape when an editor round trip is
needed. Diagrams stay with `diagramming`. Exact local commands, package sources,
licensing boundaries and bridge procedures live in the deployed skill's
`references/tools.md`, loaded on demand.

## What was considered

"Documented" means the vendor page or repository said it on 2026-09-27. None
of these bridges were authenticated or called from this Pi setup.

| Option | What it is | Decision |
| --- | --- | --- |
| Anthropic `frontend-design`, `canvas-design` | Apache-2.0 prompts, not MCPs | Ideas only. Do not vendor. Web taste is not universal |
| Anthropic `pptx` | Proprietary authoring procedure | Do not copy |
| Anthropic `webapp-testing` | Playwright skill that launches headless Chromium | Conflicts with visible-Brave-only |
| Vercel `web-design-guidelines` | MIT audit skill that fetches a remote prompt each review | Pin any rules locally. Do not install the whole repo |
| Vercel `vercel-react-native-skills` | MIT performance and platform rules | Reference only inside an Expo or React Native repo |
| Impeccable | Apache-2.0 web skill. Documents a Pi copy path. The engine may download a binary into `~/.impeccable` | Not the default. Optional in a trusted web repo after supply-chain review, with no auto-download |
| Figma MCP | Official read, and write on a narrower client list | Optional. A Pi connection is not promised |
| Penpot MCP | Official local authoring through a live plugin | Optional, and only while that plugin stays connected |
| Canva design MCP | Official Canva authoring | Optional, per user. The Dev MCP does not edit designs |
| Storybook addon MCP | Project-local component docs and tests | Optional only in a repo that already runs Storybook |

Originals stay upstream. This harness writes its own workflow. Do not run
`npx skills add` against a whole upstream repo: Vercel's also ships deploy
and token skills.

Sources read that day (upstream skill materials are not vendored):

- https://github.com/anthropics/skills (`33375500`)
- https://github.com/vercel-labs/agent-skills (`063bee94`)
- https://github.com/pbakaus/impeccable (`9d715cc4`)
- https://developers.figma.com/docs/figma-mcp-server/
- https://developers.figma.com/docs/figma-mcp-server/remote-server-installation/
- https://developers.figma.com/docs/figma-mcp-server/local-server-installation/
- https://developers.figma.com/docs/figma-mcp-server/tools-and-prompts/
- https://developers.figma.com/docs/figma-mcp-server/write-to-canvas/
- https://developers.figma.com/docs/figma-mcp-server/rate-limits-access/
- https://github.com/penpot/penpot/tree/develop/mcp
- https://www.canva.dev/docs/mcp/tools
- https://www.canva.dev/docs/mcp/usage-policy/
- https://www.canva.dev/docs/connect/mcp-server/
- https://storybook.js.org/docs/ai/mcp/overview

## Optional bridges

Turn one on only when that system is the source of truth, and only after an
explicit account and supply-chain decision. Do not add them to the global
MCP list in [pi-mcp.md](pi-mcp.md).

**Figma.** Remote URL `https://mcp.figma.com/mcp`. The desktop server, if the
desktop app is open, is `http://127.0.0.1:3845/mcp` and documents a narrower
tool set. Figma says only catalog clients can connect. `use_figma` is
remote-only and listed for named clients that do not include Pi. Write needs
a Full seat and edit permission on the file. Dev seats are documented as
read-only. Read tools are seat-limited. Quotas change, so this document does
not freeze a number. Write-to-canvas is a documented beta that Figma intends
to price by usage. `get_design_context` is a React and Tailwind sketch to
translate, not this repo's components. Figma Developer Terms apply. Pi access
was not tested and is not promised.

**Penpot.** The current server is in the Penpot repository. The standalone
`penpot/penpot-mcp` repo was archived on 2026-02-03. The model sends Plugin
API code to a plugin that must stay open. An inactive tab drops the session.
Penpot documents Brave Shields as a connection blocker. Match the package to
the Penpot version. Do not use a community Penpot server.

**Canva.** Design server `https://mcp.canva.com/mcp`, one OAuth grant per
user, no service account. Generation can spend credits. The usage policy
forbids pulling templates or Brand Kit material out of Canva to rebuild or
train on. The Dev MCP documents the Apps SDK. It does not edit designs.

**Storybook.** `@storybook/addon-mcp` on that project's dev server, commonly
`http://localhost:6006/mcp`. It can list documented components and, when
Vitest is enabled, run story tests. Manifest coverage depends on the
framework. Do not publish the server. It does not replace the visible Brave
check.

`npx -y`, unpinned MCP proxies, and a first-run binary download are
supply-chain decisions, not setup shortcuts.

## Lessons, not templates

These companies publish a method and a commercial result. The result shows a
successful company. It does not prove the method caused the result. Do not
copy their palette, type, or layout.

- Linear, [Why is quality so rare?](https://linear.app/now/why-is-quality-so-rare): craft is attention across the whole loop. Small teams iterate to "right" instead of handing a mock down a line. Incomplete work stays internal. Self-reported: profitable by year two, and more than 10,000 paying customers by year four, with effectively zero marketing spend.
- Stripe, [accessible colour systems](https://stripe.com/blog/accessible-color-systems): semantic colours with predictable contrast, tuned in a perceptual colour space rather than tinted until one pair passes. [2025 update](https://stripe.com/newsroom/news/stripe-2025-update): businesses on Stripe generated $1.9 trillion in total volume in 2025. That figure is volume, not Stripe revenue.
- Apple, [Human Interface Guidelines](https://developer.apple.com/design/human-interface-guidelines): use the platform's navigation, type, and accessibility conventions. Do not invent a second language for one screen. [FY2025 results](https://www.apple.com/newsroom/2025/10/apple-reports-fourth-quarter-results/): Apple stated fiscal 2025 revenue of $416 billion.
- Airbnb, [Building a Visual Language](https://medium.com/airbnb-design/building-a-visual-language-behind-the-scenes-of-our-airbnb-design-system-224748775e4e): one component language, adapted per platform, not one geometry on every screen. [Q2 2025 results](https://news.airbnb.com/airbnb-q2-2025-financial-results/): Q2 2025 revenue was $3.1 billion.

These lessons change the workflow: Linear motivates one lead carrying a design
through rendered iteration; Stripe motivates semantic tokens and measured colour
pairs in actual component states; Apple motivates target-runtime checks and native
controls; Airbnb motivates reusable components with platform-specific adaptations.
Commercial success does not make any of these practices universally optimal.

See [Pi workflow](pi-workflow.md) for deployment and team operation. Native/mobile
verification requires the requested SDK/runtime; a desktop preview or mobile PWA
must never be reported as a tested iOS/Android application.
