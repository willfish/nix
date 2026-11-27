import { spawn } from "node:child_process";
import { createHash, randomBytes, timingSafeEqual } from "node:crypto";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import type { IncomingMessage, ServerResponse } from "node:http";
import { createInterface } from "node:readline";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { setTimeout } from "node:timers/promises";

const modelId = "inkling-2-mini";
const contextLimit = 65536;
const defaultEffort = "medium";
const maxBodyBytes = 8 * 1024 * 1024;
const chatPath = "/v1/chat/completions";
const chatFields = [
  "stream", "stream_options", "max_tokens", "max_completion_tokens", "temperature",
  "top_p", "top_k", "min_p", "seed", "stop", "frequency_penalty", "presence_penalty",
  "repetition_penalty", "logit_bias", "logprobs", "top_logprobs", "reasoning_effort",
  "enable_thinking", "thinking_budget", "response_format", "tools", "tool_choice",
  "parallel_tool_calls",
];
const messageFields = [
  "role", "content", "name", "tool_calls", "tool_call_id", "reasoning", "reasoning_content",
];
const roles = ["system", "developer", "user", "assistant", "tool"];
const digest = (text: string) => createHash("sha256").update(text).digest();

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function pick(object: Record<string, unknown>, fields: string[]) {
  return Object.fromEntries(fields.filter((key) => Object.hasOwn(object, key))
    .map((key) => [key, object[key]]));
}

function textMessage(message: unknown) {
  if (!isObject(message) || typeof message.role !== "string" || !roles.includes(message.role)) {
    throw new Error("Invalid chat message");
  }
  const result = pick(message, messageFields);
  if (Array.isArray(message.content)) {
    result.content = message.content.map((part) => {
      if (!isObject(part) || part.type !== "text" || typeof part.text !== "string") {
        throw new Error("Only text message parts are supported");
      }
      return { type: "text", text: part.text };
    });
  } else if (message.content != null && typeof message.content !== "string") {
    throw new Error("Only text message content is supported");
  }
  return result;
}

function json(res: ServerResponse, status: number, value: unknown) {
  res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  res.end(JSON.stringify(value));
}

function error(res: ServerResponse, status: number, message: string) {
  json(res, status, { error: { message } });
}

async function chatBody(req: IncomingMessage, res: ServerResponse, modelPath: string) {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const chunk of req.iterator({ destroyOnReturn: false })) {
    size += chunk.length;
    if (size > maxBodyBytes) {
      res.setHeader("Connection", "close");
      error(res, 413, "Chat request is too large");
      return;
    }
    chunks.push(chunk);
  }
  const body: unknown = JSON.parse(Buffer.concat(chunks, size).toString("utf8"));
  if (!isObject(body) || body.model !== modelId || !Array.isArray(body.messages)) {
    throw new Error("Expected inkling-2-mini and a messages array");
  }
  return JSON.stringify({
    ...pick(body, chatFields),
    model: modelPath,
    messages: body.messages.map(textMessage),
    reasoning_effort: body.reasoning_effort ?? defaultEffort,
  });
}

async function* chatEvents(body: Readable) {
  const lines = createInterface({ input: body, crlfDelay: Infinity });
  for await (const line of lines) {
    if (line.startsWith("data: ") && line !== "data: [DONE]") {
      const event = JSON.parse(line.slice(6));
      if (Object.hasOwn(event, "model")) event.model = modelId;
      if (event.error) event.error = { message: "MLX generation failed" };
      yield `data: ${JSON.stringify(event)}\n`;
    } else {
      yield `${line}\n`;
    }
  }
}

export function createGateway(key: string, modelPath: string, upstream: typeof fetch) {
  const expected = digest(`Bearer ${key}`);
  return createServer(async (req, res) => {
    if (!timingSafeEqual(digest(req.headers.authorization ?? ""), expected)) {
      res.setHeader("WWW-Authenticate", "Bearer");
      error(res, 401, "Bearer authentication required");
      return;
    }
    if (req.method === "GET" && req.url === "/health") {
      json(res, 200, { status: "ok", model: modelId, context_limit: contextLimit });
      return;
    }
    if (req.method === "GET" && req.url === "/v1/models") {
      json(res, 200, { object: "list", data: [{ id: modelId, object: "model", owned_by: "relay" }] });
      return;
    }
    if (req.method !== "POST" || req.url !== chatPath) {
      error(res, 404, "Unknown endpoint");
      return;
    }

    let body: string | undefined;
    try {
      body = await chatBody(req, res, modelPath);
    } catch {
      if (!res.destroyed) error(res, 400, "Expected a text chat request for inkling-2-mini");
      return;
    }
    if (body === undefined || res.destroyed) return;

    const abort = new AbortController();
    res.once("close", () => abort.abort());
    try {
      const response = await upstream(chatPath, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body,
        signal: abort.signal,
      });
      if (!response.ok) {
        await response.body?.cancel();
        const status = response.status >= 400 && response.status < 500 ? response.status : 502;
        error(res, status, "MLX rejected the chat request");
      } else if (response.headers.get("Content-Type")?.startsWith("text/event-stream") && response.body) {
        res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
        res.flushHeaders();
        await pipeline(chatEvents(Readable.fromWeb(response.body)), res);
      } else {
        const completion = await response.json();
        json(res, 200, { ...completion, model: modelId });
      }
    } catch {
      if (res.headersSent) res.destroy();
      else if (!res.destroyed) error(res, 502, "MLX chat request failed");
    }
  });
}

async function verifyRuntime(upstream: typeof fetch, modelPath: string) {
  const deadline = Date.now() + 180_000;
  while (Date.now() < deadline) {
    const result = await upstream("/health", { signal: AbortSignal.timeout(2_000) }).catch(() => null);
    if (result?.ok) {
      const health = await result.json();
      if (Object.keys(health.preload_failures).length) throw new Error("MLX model preload failed");
      if (health.model_ready) {
        const request = { signal: AbortSignal.timeout(2_000) };
        const [settings, cache] = await Promise.all([
          upstream("/v1/settings", request).then((res) => res.json()),
          upstream("/v1/cache/stats", request).then((res) => res.json()),
        ]);
        if (health.loaded_model !== modelPath || health.loaded_adapter != null ||
            health.effective_context_limit !== contextLimit || !health.continuous_batching_enabled ||
            health.apc_enabled !== true || settings.current.apc_disk_enabled !== false ||
            settings.current.draft_model != null || cache.enabled !== true ||
            Object.hasOwn(cache, "disk_files")) {
          throw new Error("Loaded MLX runtime does not match the Relay settings");
        }
        return;
      }
    } else {
      await result?.body?.cancel();
    }
    await setTimeout(1_000);
  }
  throw new Error("Timed out waiting for the MLX model");
}

async function main() {
  const [runner, modelPath, keyFile] = process.argv.slice(2);
  const key = (await readFile(keyFile, "utf8")).trim();
  if (!key) throw new Error("The Relay API key is missing");
  const internalKey = randomBytes(32).toString("hex");
  const upstream: typeof fetch = (path, options) => {
    const headers = new Headers(options?.headers);
    headers.set("Authorization", `Bearer ${internalKey}`);
    return fetch(`http://127.0.0.1:18081${path}`, { ...options, headers, redirect: "error" });
  };
  const engine = spawn(runner, {
    stdio: "inherit",
    env: {
      ...process.env,
      HF_HUB_OFFLINE: "1",
      MLX_VLM_SERVER_API_KEY: internalKey,
      PREFILL_STEP_SIZE: "1024",
      MLX_VLM_MAX_NUM_SEQS: "1",
      MAX_KV_SIZE: String(contextLimit),
      APC_ENABLED: "1",
      APC_DISK_ENABLED: "0",
    },
  });
  const server = createGateway(key, modelPath, upstream);
  let stopping = false;
  function stop(exitCode: number) {
    stopping = true;
    server.close();
    server.closeAllConnections();
    engine.kill("SIGTERM");
    process.exitCode = exitCode;
  }
  process.once("SIGTERM", () => stop(0));
  process.once("SIGINT", () => stop(0));
  engine.once("error", (cause) => {
    console.error(cause);
    stop(1);
  });
  engine.once("exit", (code, signal) => {
    if (stopping) process.exit(process.exitCode);
    console.error(`MLX exited: ${code ?? signal}`);
    stop(1);
    process.exit(1);
  });
  server.once("error", (cause) => {
    console.error(cause);
    stop(1);
  });
  try {
    await verifyRuntime(upstream, modelPath);
    if (!stopping) {
      server.listen(8081, "0.0.0.0", () => {
        console.log("Inkling ready: 65536 context, one slot, 1024 prefill, RAM-only APC, no drafting");
      });
    }
  } catch (cause) {
    console.error(cause);
    stop(1);
  }
}

if (import.meta.main) {
  main().catch((cause) => {
    console.error(cause);
    process.exitCode = 1;
  });
}
