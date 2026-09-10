# Dashboards

Before drafting or updating prose, read `~/.agents/guides/documentation-relevance.md`; retain detail only when it serves this artifact's reader and purpose.

Shared guidance for CloudWatch and Grafana dashboard design, implementation and
review. AWS API details below apply to CloudWatch, not automatically to Grafana.

Source of truth: `home/config/llm/guides/dashboards.md` in the dotfiles repo.
Home Manager publishes this directory under `~/.agents/guides`,
`~/.codex/guides` and `~/.grok/guides`. Pi and other harnesses can read the
shared `~/.agents/guides/dashboards.md`; do not maintain harness-specific copies.

AWS sources checked: 2026-09-10. Recheck when changing query features, sharing,
service limits or dashboard APIs. Separate AWS requirements from our conventions.

## Contents

- [Audience and decision](#start-with-the-audience-and-decision)
- [Measurement integrity](#measurement-integrity)
- [Layout and visualisation](#layout-and-visualisation)
- [CloudWatch queries](#cloudwatch-queries-cost-and-correctness)
- [Terraform and ownership](#terraform-and-ownership)
- [Verification checklist](#verification-checklist)
- [AWS references](#aws-references)

## Start with the audience and decision

Each dashboard should answer a clear question for a named audience. Choose the
measurements to support that decision rather than filling a standard grid.

- Service operations: RED (rate, errors, duration) or the four golden signals
  (latency, traffic, errors, saturation).
- Infrastructure capacity: USE (utilisation, saturation, errors).
- Product analytics: adoption, navigation, outcomes and relevant denominators.
  Do not force RED/USE, latency percentiles or operational alarms onto a product
  question. Usage frequency is not proof of experience or expertise.

Agree metric definitions before implementation: eligible population, event being
counted, time window, units, exclusions, duplicate handling and data source.
For rates and percentages, name the denominator and handle empty populations.

## Measurement integrity

- Distinguish server-observed requests, browser-visible page views, journeys,
  browser sessions and people. Logs do not prove that a page was seen or a task
  completed. Caches, browser history and missing telemetry create gaps.
- State identity lifetime. Session identifiers do not provide durable
  cross-session browser history. Cookie resets, shared browsers and multiple
  devices limit interpretation. Never silently reuse an identity for a new use.
- Keep missing identity/history and unavailable telemetry separate from observed
  zero. Show coverage alongside rates. Do not fill gaps with zero by default.
- Document collection start and retention. A selected date range is not evidence
  that every browser has complete history for that range.
- Define treatment of bots, test traffic, assets, health checks, background
  requests, redirects, errors and refreshes. Document imperfect exclusions.
- Avoid double-counting request and journey events for the same activity.
- Distinct session counts per time bucket are not additive across buckets.
  Calculate whole-window distinct counts separately. CloudWatch distinct counts
  can be approximate at high cardinality; label that limitation.
- If defining frequency groups, inspect the distribution first. Agree thresholds,
  ties, lookback, refresh and insufficient-history handling. Use only prior
  activity for a group assigned at journey start; version the definition.

## Layout and visualisation

Flow from overview to explanation to diagnostic detail. Put a text widget first:

- Purpose, audience and owner.
- Start-here guidance and measurement definitions.
- Time window, coverage and limitations.
- Related dashboards and the investigation/definition guide.
- Approved operational targets where relevant. Do not invent a "healthy"
  baseline for product traffic or treat a proposed target as agreed.

Use consistent titles, units, time bins and colours. Do not rely on colour alone.
Keep each chart focused. Prefer separate charts for unrelated units; use a second
axis only when the relationship remains clear and both axes are labelled.

| View | Use |
| --- | --- |
| Time series | Trends, counts, rates and latency distributions |
| Summary value/table | Whole-window totals and coverage with explicit labels |
| Bar chart | Ranked categories or a frequency distribution |
| Pie chart | A few mutually exclusive proportions; avoid when trends matter |
| Log table | Bounded diagnostic details, below aggregate charts |

Stack only mutually exclusive, additive series when the total is meaningful.
Never stack percentiles, percentages or overlapping distinct populations.

Use newest-first ordering for recent failures, but oldest-first for a session
trace. Limit diagnostic results (usually 20-50 rows) and state truncation. Avoid
raw messages, query strings, form data or authentication details in shared views.

CloudWatch uses a 24-column grid. Common chart sizes are 6, 8 or 12 columns wide
and 6 rows high. Give explanatory text enough height to avoid clipping. These
are conventions, not reasons to squeeze in unnecessary widgets.

## CloudWatch queries, cost and correctness

- Select only necessary log groups and the narrowest useful time window.
  A shared platform log group needs an explicit, verified application selector.
- Each dashboard refresh starts new Logs Insights queries. Choose refresh and
  default window based on scan cost and data freshness, not just traffic rate.
  Prefer manual or modest refresh for exploratory product dashboards.
- Ordinary result limits do not establish a scan-cost budget. Cancel console
  queries when finished; do not assume closing the page cancels them.
- Use `stats` with a time bin for time series. Keep units explicit, for example
  `bin(5m)` rather than assuming a seconds value accepts any duration.
- Aggregation drops fields not retained in its output. Carry timestamps and
  grouping keys explicitly through multi-stage aggregations.
- Do not use `dedup` followed by `stats`: only `limit` can follow `dedup`.
  Use an initial grouping aggregation to collapse duplicate request IDs before
  aggregating counts, and account for missing IDs separately.
- Verify current syntax and quotas against AWS docs rather than hard-coding old
  limits in guides. Test real queries in the intended region and log class.
- Never use session IDs, request IDs, IP addresses or unbounded paths as custom
  metric dimensions. High cardinality creates cost and operational risks.
- Dashboard variables can provide an explicit drill-down filter. Verify whether
  property or pattern substitution fits the widget. Shared-dashboard viewers
  cannot change variable values; do not design access around that capability.

Example trend, after the application's eligibility filter:

```text
stats count(*) as page_requests by bin(5m)
```

Example duplicate collapse for events with valid request IDs, after eligibility
filtering. This example counts requests only; it is not a session-count query:

```text
filter ispresent(request_id) and request_id != ""
| stats earliest(@timestamp) as requested_at by request_id
| stats count(*) as page_requests by datefloor(requested_at, 5m)
```

Query examples still require staging validation against the actual log schema.
Terraform validation does not execute Logs Insights queries.

## Terraform and ownership

- Keep dashboards and reusable query definitions version-controlled in the
  owning repository. Console edits are not the maintained source of truth.
- Follow existing module conventions and provider versions. Use `jsonencode`,
  shared source/eligibility locals and explicit widget regions.
- Name dashboards by purpose and environment, for example
  `Frontend-Page-Visits-staging`. One service can have separate product and
  operational dashboards; link them rather than mixing unrelated questions.
- Use the existing deployment configuration. Do not introduce new IAM access,
  public sharing, retention changes or AWS applies without approval.
- Follow least privilege for dashboard and underlying log/query access.
  Pseudonymous identifiers still need appropriate access and privacy controls.
- Keep generated Terraform README sections generated. Put design and counting
  contracts in maintained documentation outside those sections.
- Review dashboards when features or metrics change. Remove obsolete views and
  assign an owner; avoid unmanaged copies that drift from the source.

## Verification checklist

1. Every widget answers a question for the intended audience.
2. Counting rules, units, denominators, identity scope and exclusions are explicit.
3. Missing data, unknown history and approximation are visible.
4. Charts have readable titles/axes, appropriate ordering and no clipped text.
5. Queries cannot mix applications or double-count the same activity.
6. Synthetic cases cover duplicates, missing IDs, boundary timestamps and empty
   populations. Cohort logic additionally covers ties and prior-only assignment.
7. Run Terraform formatting, validation and relevant tests from the repository
   environment. Inspect the plan for unrelated or destructive changes.
8. With authorised AWS access, run queries over a bounded staging window and
   inspect actual rendering, empty states, query duration and bytes scanned.
   Publish aggregate evidence without identifiers; record any unverified claims.
9. Retention, access, refresh costs and deployment ownership are understood.

## AWS references

- [Dashboard design and maintenance](https://docs.aws.amazon.com/prescriptive-guidance/latest/implementing-logging-monitoring-cloudwatch/cloudwatch-dashboards-visualizations.html)
- [Current CloudWatch dashboards and cross-account capabilities](https://docs.aws.amazon.com/AmazonCloudWatch/latest/monitoring/CloudWatch_Dashboards.html)
- [Logs Insights syntax and query-cost guidance](https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/CWL_QuerySyntax.html)
- [Aggregation and distinct-count semantics](https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/CWL_QuerySyntax-Stats.html)
- [Deduplication restrictions](https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/CWL_QuerySyntax-Dedup.html)
- [Dashboard variables and sharing restrictions](https://docs.aws.amazon.com/AmazonCloudWatch/latest/monitoring/cloudwatch_dashboard_variables.html)
- [Metric-filter cardinality and costs](https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/FilterAndPatternSyntaxForMetricFilters.html)

Older prescriptive guidance describes cross-account log limitations that newer
CloudWatch documentation supersedes. Use current service/API documentation for
capability claims, not older architecture prose in isolation.
