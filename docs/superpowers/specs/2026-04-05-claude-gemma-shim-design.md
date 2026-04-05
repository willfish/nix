# Claude Gemma Shim Design

**Date:** 2026-04-05

## Goal

Provide a localhost-only way to run Claude Code against the local Gemma 4 llama.cpp server on the RTX 5090, with enough Anthropic API compatibility for real Claude Code tool use rather than a text-only demo.

## Scope

In scope:
- A localhost-only shim that exposes the Anthropic-style endpoint Claude Code expects.
- Translation between Claude Code requests and llama.cpp's OpenAI-compatible API.
- Streaming support.
- Tool definition, tool call, and tool result round-tripping.
- A `claude-gemma` wrapper command that points Claude Code at the shim.
- Home Manager packaging so the wrapper is reproducible in this dotfiles repo.

Out of scope:
- General multi-provider gateway behaviour.
- Network exposure beyond `127.0.0.1`.
- Long-term persistence, auth systems, or multi-user access.
- Full Anthropic API emulation beyond the subset Claude Code actually uses.

## Constraints

- Must bind to `127.0.0.1` only.
- Must be narrow in scope and easy to audit.
- Must avoid LiteLLM and similar broad third-party gateway dependencies.
- Must preserve the normal `claude-code` package untouched.
- Must be honest about the real risk: protocol translation is manageable, but Gemma's tool-use quality may still be the limiting factor.

## Recommended approach

Build a small local shim plus a launcher:

1. `claude-gemma-shim`
   - listens on `127.0.0.1`
   - implements the small Anthropic-style surface Claude Code needs, starting with `/v1/messages`
   - translates requests to `http://127.0.0.1:8080/v1/chat/completions`
   - translates normal text, streaming events, and tool-use payloads back into Anthropic-style responses

2. `claude-gemma`
   - starts or connects to the shim
   - exports the Claude Code gateway environment variables
   - execs the packaged `claude` binary from the existing `claude-code` package

This keeps the implementation focused on one local use case instead of trying to reproduce a generic API gateway.

## Architecture

### Components

#### 1. Wrapper command
A shell script package that:
- resolves the packaged `claude` binary
- points Claude Code at the local shim using Anthropic gateway environment variables
- passes through all CLI arguments unchanged

#### 2. Shim server
A small local HTTP service, likely in Python because it is already available and easy to audit in dotfiles.

Responsibilities:
- accept Claude Code requests
- normalise Anthropic-style messages into llama.cpp-compatible chat messages
- map tool schemas from Anthropic format into the subset llama.cpp accepts
- forward requests to the local `llm-gemma` server
- convert llama.cpp responses back into Anthropic message blocks and streaming events

#### 3. Optional shim launcher behaviour
If the shim is not already running, the wrapper can either:
- fail with a clear message telling the user to start it, or
- start it automatically in the background

Recommended first version: explicit start command or simple auto-start if no listener is present.

## Request and response flow

### Text response path
1. Claude Code sends `POST /v1/messages` to the shim.
2. Shim extracts system prompt, conversation history, sampling settings, and tools.
3. Shim converts those into llama.cpp chat completion payload shape.
4. Shim sends request to local Gemma server.
5. Shim converts the result into Anthropic-style `message` output.

### Tool-use path
1. Claude Code includes tool definitions in the Anthropic request.
2. Shim converts them into llama.cpp-compatible tool definitions.
3. Gemma returns either plain text or a structured tool call.
4. Shim emits Anthropic-style `tool_use` content blocks.
5. Claude Code runs the requested tool locally.
6. Claude Code sends tool results back as Anthropic `tool_result` blocks.
7. Shim converts those back into llama.cpp-compatible follow-up messages.
8. Gemma continues the turn.

## API surface

Initial target surface:
- `POST /v1/messages`
- streaming for `POST /v1/messages`
- lightweight health endpoint for the wrapper to probe readiness

Avoid implementing unused endpoints until there is hard evidence Claude Code needs them.

## Streaming design

Streaming matters because Claude Code expects incremental output.

The shim should:
- consume llama.cpp streaming responses if available
- map incremental text deltas into Anthropic-style SSE events
- emit clear event boundaries for message start, content deltas, tool-use blocks, message stop, and errors

If exact event parity is difficult, the shim should still preserve the event shapes Claude Code actually relies on. That means we should test against the real CLI early rather than overdesigning from docs alone.

## Error handling

The shim should fail loudly and specifically.

Cases to handle:
- local Gemma server not running
- malformed request shape from Claude Code
- unsupported tool payload shape
- malformed tool call returned by Gemma
- timeout from llama.cpp
- Claude Code requesting Anthropic features we have not implemented

Error messages should identify which side failed:
- Claude request parse failure
- shim translation failure
- llama.cpp upstream failure
- unsupported protocol feature

## Security model

- bind only to `127.0.0.1`
- use a fixed local token only so Claude Code has something to send
- do not expose on LAN
- do not add generic remote forwarding or proxying
- do not store prompts or tool results unless debugging is explicitly enabled

The main security value comes from narrow scope and local-only binding, not from pretending this is a hardened public gateway.

## Testing strategy

We need to test protocol behaviour, not just unit logic.

### Phase 1: translation sanity
- canned Anthropic-style request in
- verify valid llama.cpp payload out
- canned llama.cpp text response back into Anthropic format

### Phase 2: streaming sanity
- verify Claude Code can receive incremental output from the shim

### Phase 3: tool-use proof
- define one trivial tool
- verify Gemma emits a tool call that Claude Code accepts
- verify tool result round-trip works
- verify the assistant can continue after the tool result

### Phase 4: real CLI validation
- run `claude-gemma` against the local shim
- confirm interactive session starts
- confirm at least one real tool invocation works end-to-end

## Risks

### 1. Tool-use quality
This is the biggest real risk. Even if the shim is technically correct, Gemma may not be reliable enough at tool calling for Claude Code to feel frontier-grade.

### 2. Protocol drift
Claude Code may change the exact Anthropic event or request shapes it depends on. Keeping the shim narrow reduces maintenance, but does not eliminate drift risk.

### 3. Hidden assumptions in Claude Code
There may be subtle expectations around usage reporting, stop reasons, or message block ordering that only show up under the real client.

## Success criteria

The design is successful when:
- `claude-gemma` launches Claude Code against the local shim
- the shim talks to local Gemma on `127.0.0.1`
- normal text responses work
- at least one real Claude Code tool call works end-to-end
- the system remains localhost-only and easy to inspect

## Recommendation

Proceed with a deliberately small localhost-only Anthropic-to-llama.cpp shim plus a `claude-gemma` wrapper. Test real Claude Code tool use early, because model behaviour - not transport - is the main uncertainty.
