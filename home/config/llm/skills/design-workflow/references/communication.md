# Presentations and marketing

Apply `~/.agents/guides/documentation-relevance.md` to prose.
Use `references/tools.md` for executable render/export routes and limitations.
Creation, repairs, round-trip edits and persisted handoffs here apply only to
implementation mode. Advice/critique inspect existing artifacts and report in
the response without editing; mark checks that need edits as not run.

## Decide the delivery contract first

Name audience, setting/channel, desired understanding or action, brand, content
sources, dimensions, language and editable format. Distinguish a live talk from
a leave-behind, a product explanation from an advertisement, and source-code
editability from editing in PowerPoint/Keynote/Canva. Existing brand/templates
win over a new house style. Check asset/font licences before embedding.

## Decks

Build a narrative: question or tension, evidence, implication, recommendation and
next step. Use assertion-style slide titles when the story warrants them. Each
slide should have one clear takeaway, not one arbitrary number of bullet points.
Use visual hierarchy and a consistent grid; vary composition when content needs
it. A title slide, evidence slide and decision slide need not look identical.

Use readable presentation-scale type and labels, adequate contrast and enough
space for actual content. Do not shrink everything to fix overflow. Charts need
units, labelled categories, source and a truthful scale. Keep numbers and chart
structure editable; do not bake them into a screenshot. Put detail in notes or
an appendix when the live presentation does not need it. Supply speaker notes
when delivery requires them and an accessible text outline/reading order.

Default to Typst source plus PDF and page-image previews for code-maintained
decks. Keep existing Beamer/Pandoc projects in their toolchain. If GUI-editable
PPTX is required, use native shapes/text/charts through python-pptx and inspect
with LibreOffice or the recipient's editor. Typst PDF is not a substitute for
PPTX. Do not copy proprietary third-party document skills.

Render and read every page at intended presentation size. Check typography,
clipping, alignment, narrative sequence, source/footnote readability and final
page count. Reopen/edit a heading and chart value, re-export and verify changes.
Do not claim an accessible PDF or PowerPoint round trip from a successful render;
check tagging/reading order and the actual editor where that contract matters.

## Marketing

Start with a specific audience problem, substantiated proposition and one clear
next action. Keep the product recognisable; do not make every artifact look like
a generic SaaS hero. Compare two visual directions when the brief is open, then
commit to one hierarchy, type family/scale, palette and graphic approach.

Prefer original editable SVG for vector graphics and local HTML/CSS for landing
pages. Use images only when they improve communication, with provenance and
appropriate rights. Do not invent social proof, performance claims, fake product
screens, urgency or scarcity. Label fictional campaigns and synthetic figures.
Avoid dark patterns, concealed opt-outs and confusing consent.

Design for the channel's actual dimensions, cropping and viewing context. Check
at target size, thumbnail size where relevant, and with long/translatable copy.
Keep safe margins for crop/print. Supply alt text/caption alongside image exports.
For print, confirm bleed, colour profile and printer requirements; ordinary RGB
SVG/PDF is not automatically press-ready. For web, load `references/interfaces.md` and test
the CTA destination, keyboard path, forms and error recovery.

Hand off editable sources, exports, font/asset provenance, exact export commands
and remaining delivery checks. Publishing, purchasing stock, consuming credits
and editing shared brand files are separate authorization gates.
