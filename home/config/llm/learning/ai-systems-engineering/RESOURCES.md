# AI Systems Engineering Resources

## Knowledge

- [OpenAI Agents SDK guide](https://developers.openai.com/api/docs/guides/agents)
  Use for: the current OpenAI surface area around agent definitions, orchestration, guardrails, results/state, observability integrations, and agent workflow evaluation.
- [OpenAI Function calling guide](https://developers.openai.com/api/docs/guides/function-calling)
  Use for: tool schema design, strict function calling, parallel tool calls, and the boundary between model decisions and application-owned actions.
- [OpenAI Evals guide](https://developers.openai.com/api/docs/guides/evals)
  Use for: turning quality claims into testable criteria, especially small regression suites for model output and workflow behavior.
- [Model Context Protocol introduction](https://modelcontextprotocol.io/docs/getting-started/intro)
  Use for: understanding MCP as a standard connection layer between AI applications and external data, tools, and workflows.
- [Model Context Protocol specification](https://modelcontextprotocol.io/specification/2025-11-25)
  Use for: authoritative protocol behavior when building or evaluating MCP clients and servers.
- [LangGraph overview](https://docs.langchain.com/oss/python/langgraph/overview)
  Use for: production agent runtime concepts, especially durable execution, persistence, streaming, human-in-the-loop, and memory.
- [LangGraph persistence](https://docs.langchain.com/oss/python/langgraph/persistence)
  Use for: checkpointing, resumability, short-term memory, and long-term memory in stateful agent workflows.
- [LangChain human-in-the-loop docs](https://docs.langchain.com/oss/python/langchain/human-in-the-loop)
  Use for: approval policies around risky tool calls and interrupt/resume workflow design.
- [OpenTelemetry GenAI semantic conventions pointer](https://opentelemetry.io/docs/specs/semconv/gen-ai/)
  Use for: locating the current GenAI semantic convention repository and avoiding obsolete observability vocabulary.
- [OpenTelemetry semantic conventions: GenAI docs directory](https://github.com/open-telemetry/semantic-conventions/tree/main/docs/gen-ai)
  Use for: vendor-neutral tracing, metrics, events, exception, agent-span, and MCP observability vocabulary.

## Wisdom (Communities)

- [OpenAI Developer Community](https://community.openai.com/)
  Use for: practical reports from builders using current OpenAI APIs in production-like systems.
- [LangChain Forum](https://forum.langchain.com/)
  Use for: LangGraph and agent orchestration implementation questions, including persistence and human review patterns.
- [Model Context Protocol GitHub organization](https://github.com/modelcontextprotocol)
  Use for: real MCP server examples, protocol discussions, and implementation patterns.
- [OpenTelemetry GitHub organization](https://github.com/open-telemetry)
  Use for: instrumentation discussions and emerging GenAI observability conventions.

## Gaps

- A single vendor-neutral "AI systems engineering" curriculum is still immature. This workspace compensates by combining primary docs with production-style exercises.
- Good public examples of complete AI systems with evals, guardrails, traces, cost controls, and fallbacks are still sparse. Portfolio work should make those decisions explicit.
