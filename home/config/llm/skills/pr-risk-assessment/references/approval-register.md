# Approval register and PR updates

Read `~/.agents/guides/documentation-relevance.md` before writing. Use short,
business-appropriate wording: what changes, who could be affected and what
requires discussion. Do not paste code, test logs or an investigation diary.

## Select the register and authority

For trade-tariff medium/high-risk PRs, use the designated
[Pull Requests Approval register](https://transformuk.atlassian.net/wiki/spaces/HO/pages/22995107847/Pull+Requests+Approval).
For other projects, use their designated register; do not add unrelated work to
this page. Read the live page and its current columns rather than copying a
private register into public skill files or treating this link as a frozen schema.

A risk assessment or catalogue match alone does not authorize external writes.
If the user has asked to apply the rating and record the PR, that authorization
covers the requested PR/register updates together; do not ask again for each
routine step. Otherwise, prepare the entry and obtain publishing authorization
through the structured question mechanism before changing external state.

## Apply the decision safely

1. Read the current PR, labels, state and risk section. Confirm repository and
   PR URL; a bare PR number is not unique across repositories.
2. For an upward reassessment, remove low-risk auto-merge eligibility first.
   Apply exactly one matching risk label, preserving unrelated labels. Update
   the risk marker and reason in the PR body without overwriting other sections.
3. For medium/high, find the register entry by the full PR URL. Add it once if
   absent, or update its current rating without duplicating it or erasing genuine
   approval history. Do not add new low-risk entries unless the project requires
   them; a downgrade of an existing entry should retain its audit trail.
4. Check merge state before labelling the review. For an **already-merged PR**,
   use today's assessment date and say **Retrospective reassessment from [old]
   to [new]**, with the reason and evidenced merge date where relevant. An open
   PR gets a normal current assessment even if its earlier rating changes.
   If asked what went live, verify production deployment evidence separately;
   merge or a development deployment does not establish that. Never backdate
   the entry or present a retrospective review as prior approval.
5. Match the page's current columns. Include a concise change summary, the
   concrete business impact, PR/owner/ticket links and the required next action.
   Leave the decision blank or explicitly pending, according to the register's
   conventions, unless an authorized decision is evidenced. CI, bot approval
   and a risk rating do not prove the required human approval occurred.
6. Use the Confluence workflow for access and writes: prefer configured MCP,
   read the latest storage body/version/parent, preserve unrelated content and
   macros, and use an optimistic version update. Keep private content and
   credentials out of Git and logs.
7. Read back both systems. Verify the risk marker and sole risk label, exactly
   one register entry, its rating and retrospective wording, unchanged page
   identity/parent, and preserved decision history. Account for harmless macro
   normalization without overlooking changed content. After an uncertain write,
   inspect before retrying; never blindly append another entry.

If one update succeeds and the other fails, report the partial state and complete
only the missing authorized step. Never claim the decision is synchronized until
both readbacks agree. Do not merge, bypass protections, change credentials or
mark a register entry approved merely to complete this workflow.
