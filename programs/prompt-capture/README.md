# Prompt capture adapter

`prompt-capture-mitm` implements the owned capture add-on in C and embeds the
pinned upstream mitmproxy runtime. The Bash `prompt-capture` command retains its
Pi-only CLI, proxy lifecycle, private state, capture lock and certificate-only
trust bundle. Real prompt capture remains opt-in.

The adapter registers directly with `DumpMaster`, since mitmproxy's script
loader accepts only Python source. CPython and mitmproxy remain dependencies;
there is no generated Python add-on. The adapter retains request JSONL fields,
credential-header redaction, usage extraction, optional response bodies,
incremental SSE decoding and WebSocket direction. Streaming callbacks always
return the original bytes, including when recording fails. Diagnostics do not
include captured content.

Build and run the manual, program-local checks with synthetic local traffic:

```sh
nix develop .#prompt-capture-mitm --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/prompt-capture-checks programs/prompt-capture \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#prompt-capture-mitm --command meson compile -C /tmp/prompt-capture-checks
/tmp/prompt-capture-checks/prompt-capture-checks
```

These checks are not installed or registered with package builds or hooks. They
use disposable state and ports, not real model requests or existing captures.
