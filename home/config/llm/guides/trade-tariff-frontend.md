# Trade Tariff Frontend Testing

## Authentication

The frontend requires password authentication in development.

- Password location: `.env.development` file
- Default password: `dev123`

## Testing with Python

Use this pattern to authenticate and test the frontend:

```python
import urllib.request
import urllib.parse
import http.cookiejar
import re

# Setup cookie jar for session persistence
cj = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))

# Get login page and extract CSRF token
login_resp = opener.open('http://localhost:3001/basic_sessions/new')
login_html = login_resp.read().decode('utf-8')
csrf = re.search(r'name="authenticity_token" value="([^"]+)"', login_html).group(1)

# Login
data = urllib.parse.urlencode({
    'authenticity_token': csrf,
    'basic_session[password]': 'dev123',
    'basic_session[return_url]': '/uk/search?q=horse&internal_search=true'
}).encode()
opener.open(urllib.request.Request('http://localhost:3001/basic_sessions', data=data, method='POST'))

# Now make authenticated requests
resp = opener.open('http://localhost:3001/uk/search?q=horse&internal_search=true')
html = resp.read().decode('utf-8')
```

## Interactive Search Q&A Flow

The interactive search form uses GET method with these parameters:

- `q` - search query
- `internal_search` - set to 'true'
- `request_id` - UUID for tracking the session
- `answers[][question]` - question text
- `answers[][options]` - JSON array of options
- `answers[][answer]` - selected answer

Example URL building:
```python
params = [
    ('q', 'horse'),
    ('internal_search', 'true'),
    ('request_id', request_id),
    ('answers[][question]', 'What type of product?'),
    ('answers[][options]', '["Option A", "Option B"]'),
    ('answers[][answer]', 'Option A'),
]
url = 'http://localhost:3001/uk/search?' + urllib.parse.urlencode(params)
```

## Verifying Confidence Display

Check for confidence indicators in the HTML:
```python
if 'confidence-indicator' in html:
    strong = html.count('Strong match')
    good = html.count('Good match')
    possible = html.count('Possible match')
    unknown = html.count('>Unknown<')
```

## Key Files

- Question page: `app/views/search/interactive_question.html.erb`
- Results page: `app/views/search/interactive_results.html.erb`
- Confidence helper: `app/helpers/search_results_helper.rb`
- Search model: `app/models/search.rb`
- Internal search result: `app/models/search/internal_search_result.rb`

## Ports

- Frontend: 3001
- Backend: 3000

## tmux Sessions

- `backend` - Rails backend server
- `frontend` - Rails frontend server
