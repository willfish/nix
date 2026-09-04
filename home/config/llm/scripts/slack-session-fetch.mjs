// The pinned Slack MCP server accepts a bearer but has no session-cookie option.
// This preload supplies the matching cookie only to its authenticated Slack API requests.
export function withSlackSession(fetch, token, cookie) {
  if (!token || !cookie || /[^\x21-\x7e]/.test(token + cookie)) {
    throw new Error("Valid Slack session credentials are required");
  }
  return function sessionFetch(input, init) {
    const request = input instanceof Request ? input : null;
    const url = new URL(request ? request.url : input);
    if (url.origin !== "https://slack.com" || !url.pathname.startsWith("/api/")) {
      return fetch(input, init);
    }
    const headers = new Headers(init?.headers ?? request?.headers);
    if (headers.get("authorization") !== `Bearer ${token}`) {
      return fetch(input, init);
    }
    headers.set("cookie", `d=${cookie}`);
    return fetch(input, { ...init, headers });
  };
}

globalThis.fetch = withSlackSession(
  globalThis.fetch.bind(globalThis),
  process.env.SLACK_XOXC,
  process.env.SLACK_COOKIE_D,
);
