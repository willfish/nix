# AWS access portal MCP

Rust stdio service for the existing five portal tools. It uses the configured
visible Brave debugger at `127.0.0.1:9222` and the fixed regional portal API.
It does not launch a browser, focus tabs or expose credential values in replies.

Login reuses a valid session or creates one background tab, submits the existing
AWSUI selectors, permits at most two MFA attempts and closes its owned tab before
returning. Credential fields are filled only on the configured portal/sign-in
origins. CDP debugger URLs must remain on the configured endpoint; API redirects
are rejected rather than forwarding bearer headers elsewhere.

Exports retain the private env-file format and five-minute cache margin.
Production administrator approval happens before cache access as well as before
a fresh fetch. Only the local fuzzel decision can approve; the production binary
has no approval environment hook. The installed wrapper also clears the legacy
hook variables and supplies fuzzel. Source the returned file without printing it.

## Manual verification

```sh
cargo test --locked --manifest-path programs/aws-access-portal/Cargo.toml
node --test programs/aws-access-portal/tests/dom.test.mjs
```

Tests are program-local and are not registered in package builds, flake checks or
hooks. They use fake portal/approval objects, disposable credential files, a local
HTTP/WebSocket peer and synthetic DOM controls. They never sign into AWS, answer
a live approval prompt or read real secrets. Live authentication and export still
require the AWS skill's explicit user authorization.
