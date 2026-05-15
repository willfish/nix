# Jira API Guide

## Connection

- Instance: `transformuk.atlassian.net`
- API: **v3 only** (v2 returns 410 Gone)
- Credentials: `~/.env` contains `JIRA_EMAIL` and `JIRA_API_TOKEN` — always `source ~/.env` before use
- Auth: Basic auth via `curl -u "$JIRA_EMAIL:$JIRA_API_TOKEN"`

## Common API patterns

**Search issues (POST — not GET):**
```bash
source ~/.env && curl -s -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  -X POST "https://transformuk.atlassian.net/rest/api/3/search/jql" \
  -H "Content-Type: application/json" \
  -d '{"jql": "project = AI AND parent = AI-137 ORDER BY key ASC", "fields": ["summary", "status"], "maxResults": 20}'
```
Note: Unbounded JQL (no project/filter) is rejected. Always include a search restriction.

**Fetch a single issue:**
```bash
source ~/.env && curl -s -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-137"
```

**Update an issue (PUT):**
```bash
source ~/.env && curl -s -X PUT -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-137" \
  -H "Content-Type: application/json" \
  -d '{"fields": {"summary": "New title", "description": {...ADF...}}}'
```

**Create an issue (POST):**
```bash
source ~/.env && curl -s -X POST -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue" \
  -H "Content-Type: application/json" \
  -d '{"fields": {"project": {"key": "AI"}, "parent": {"key": "AI-137"}, "issuetype": {"id": "10638"}, "summary": "...", "description": {...ADF...}}}'
```

**Transition an issue (e.g. close it):**
```bash
# First get available transitions
source ~/.env && curl -s -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-150/transitions"

# Then transition (31 = Done in the AI project)
source ~/.env && curl -s -X POST -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-150/transitions" \
  -H "Content-Type: application/json" \
  -d '{"transition": {"id": "31"}}'
```

**Add a comment:**
```bash
source ~/.env && curl -s -X POST -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-150/comment" \
  -H "Content-Type: application/json" \
  -d '{"body": {"type": "doc", "version": 1, "content": [{"type": "paragraph", "content": [{"type": "text", "text": "Comment text here."}]}]}}'
```

**Attach a file:**
```bash
source ~/.env && curl -s -X POST -u "$JIRA_EMAIL:$JIRA_API_TOKEN" \
  "https://transformuk.atlassian.net/rest/api/3/issue/AI-137/attachments" \
  -H "X-Atlassian-Token: no-check" \
  -F "file=@/path/to/file.png"
```

## AI project specifics

- Project key: `AI`
- Project type: next-gen (team-managed, simplified)
- Hierarchy: Epic (level 1) → Story/Task/Bug/Feature (level 0) → Subtask (level -1)
- No native Feature > Epic hierarchy — Feature is at the same level as Story
- Issue type IDs: Epic `10639`, Story `10638`, Task `10636`, Bug `10637`, Subtask `10640`, Feature `10706`
- Child stories link to epics via the `parent` field: `"parent": {"key": "AI-137"}`
- Transition IDs: To Do `11`, In Progress `21`, Done `31`

## Atlassian Document Format (ADF)

Descriptions are NOT markdown — they use ADF (a JSON document format). Key node types:

```
paragraph, heading (attrs.level), bulletList, orderedList, listItem,
panel (attrs.panelType: info/note/warning/success/error),
expand (attrs.title — collapsible section),
table, tableRow, tableHeader, tableCell,
text (with optional marks: strong, em, code, link)
```

Always build descriptions programmatically in Python to avoid ADF syntax errors.

## Known API quirks and errors

### Search returns IDs but not fields
The POST `/rest/api/3/search/jql` endpoint can behave inconsistently:
- Without `fields` parameter: returns issues with only internal numeric IDs (e.g., `{"id": "40179"}`)
- With `fields` parameter: may return empty `issues` array for the same query

This appears to be a permissions issue where the API can "see" issues exist but cannot access their fields.

### "Issue does not exist or you do not have permission"
Direct issue access via `/rest/api/3/issue/{key}` can fail even when:
- Authentication succeeds (`/rest/api/3/myself` returns your user)
- Search endpoints find the issues

**Workaround**: If API access fails, use the Jira web UI directly. The permission model for API access may differ from web access.

### GET search endpoint removed
The GET `/rest/api/3/search` endpoint returns:
```json
{"errorMessages": ["The requested API has been removed. Please migrate to the /rest/api/3/search/jql API..."]}
```
Always use POST to `/rest/api/3/search/jql` instead.

### Unbounded JQL rejected
Queries without restrictions (e.g., `ORDER BY created DESC`) return:
```json
{"errorMessages": ["Unbounded JQL queries are not allowed here. Please add a search restriction to your query."]}
```
Always include a filter like `project = AI` or `created > -7d`.

### Empty transition response
Transition POST requests may return empty response even on success. Verify by fetching the issue status afterward.

## Extracting text from ADF descriptions

When reading issue descriptions, pipe the JSON through a recursive text extractor — don't try to use jq for nested ADF content. Save the response to a temp file first, then use Python:

```python
import json

with open("/tmp/issue.json") as f:
    d = json.load(f)

def extract_text(node):
    if isinstance(node, dict):
        ntype = node.get('type', '')
        text = node.get('text', '')
        children = node.get('content', [])
        result = ''
        if ntype == 'paragraph': result += '\n'
        elif ntype == 'heading':
            level = node.get('attrs', {}).get('level', 1)
            result += '\n' + '#' * level + ' '
        elif ntype == 'listItem': result += '- '
        elif ntype in ('orderedList', 'bulletList'): result += '\n'
        if text: result += text
        for child in children: result += extract_text(child)
        return result
    return ''

print(extract_text(d['fields']['description']))
```
