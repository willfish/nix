// CI sets SLACK_MCP_WRAPPER to the built immutable Home Manager wrapper.
// For local development, SLACK_MCP_MODULES may identify the pinned node_modules.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import test from "node:test";
import { fileURLToPath } from "node:url";

const token = "xoxc-synthetic-fixture";
const cookie = "xoxd-synthetic-fixture";
process.env.SLACK_XOXC = token;
process.env.SLACK_COOKIE_D = cookie;
let shim = fileURLToPath(new URL("../home/config/llm/scripts/slack-session-fetch.mjs", import.meta.url));
let modules = process.env.SLACK_MCP_MODULES;
if (process.env.SLACK_MCP_WRAPPER) {
  // Read static paths only. Executing the wrapper would read real credentials.
  const wrapper = await readFile(process.env.SLACK_MCP_WRAPPER, "utf8");
  const imports = [...wrapper.matchAll(/--import (\/nix\/store\/[^\s]+-slack-session-fetch\.mjs)/g)];
  const entries = [...wrapper.matchAll(/(\/nix\/store\/[^\s]+\/node_modules)\/\.bin\/mcp-server-slack/g)];
  assert.equal(imports.length, 1, "Built Slack wrapper must preload exactly one session transport");
  assert.equal(entries.length, 1, "Built Slack wrapper must identify exactly one pinned server");
  shim = imports[0][1];
  modules = entries[0][1];
}
const { withSlackSession } = await import(shim);

test("session transport preserves Request options and existing headers", async () => {
  const input = new Request("https://slack.com/api/users.list", {
    method: "POST", body: "{}",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
  });
  const controller = new AbortController();
  const response = new Response("{}");
  let observed;
  const fetch = withSlackSession(async (...args) => {
    observed = args;
    return response;
  }, token, cookie);
  assert.equal(await fetch(input, { signal: controller.signal }), response);
  assert.equal(observed[0], input);
  assert.equal(observed[1].signal, controller.signal);
  assert.equal(observed[1].headers.get("authorization"), `Bearer ${token}`);
  assert.equal(observed[1].headers.get("content-type"), "application/json");
  assert.equal(observed[1].headers.get("cookie"), `d=${cookie}`);
  assert.equal(input.headers.get("cookie"), null);
  assert.equal(await input.text(), "{}");
});

test("session cookie is not attached to other destinations or bearer tokens", async () => {
  const cases = [
    ["http://slack.com/api/users.list", token],
    ["https://example.invalid/api/users.list", token],
    ["https://slack.com/other", token],
    ["https://slack.com/api/users.list", "xoxb-other-token"],
  ];
  for (const [url, bearer] of cases) {
    const init = { headers: { Authorization: `Bearer ${bearer}` } };
    const fetch = withSlackSession(async (input, options) => {
      assert.equal(input, url);
      assert.equal(options, init);
      assert.equal(new Headers(options.headers).get("cookie"), null);
    }, token, cookie);
    await fetch(url, init);
  }
});

test("invalid session credentials fail without including their values", () => {
  for (const [bearer, session] of [["", cookie], [token, ""], [token, cookie + "\n"], [token, cookie + "\0"]]) {
    assert.throws(() => withSlackSession(() => {}, bearer, session), {
      message: "Valid Slack session credentials are required",
    });
  }
});

test("pinned Slack MCP sends both session credentials and preserves its tools", { timeout: 15000 }, async (t) => {
  assert.ok(modules, "Set SLACK_MCP_WRAPPER or SLACK_MCP_MODULES to a built Slack MCP runtime");
  const temp = await mkdtemp(path.join(tmpdir(), "slack-mcp-auth-"));
  t.after(() => rm(temp, { recursive: true, force: true }));
  const recorded = path.join(temp, "headers.json");
  const preload = path.join(temp, "mock-transport.mjs");
  await writeFile(preload, `
    import { createRequire } from "node:module";
    import { writeFileSync } from "node:fs";
    const require = createRequire(process.env.SLACK_MCP_MODULES + "/undici/index.js");
    const { MockAgent, setGlobalDispatcher } = require("undici");
    const agent = new MockAgent();
    agent.disableNetConnect();
    setGlobalDispatcher(agent);
    agent.get("https://slack.com").intercept({
      path: /^\\/api\\/conversations.list\\?/, method: "GET",
    }).reply(200, ({ headers }) => {
      writeFileSync(process.env.SLACK_TEST_HEADERS, JSON.stringify(headers));
      return { ok: true, channels: [] };
    });
  `);
  const server = spawn(process.execPath, [
    "--import", preload,
    "--import", shim,
    path.join(modules, "@modelcontextprotocol/server-slack/dist/index.js"),
  ], {
    env: {
      ...process.env,
      SLACK_MCP_MODULES: modules,
      SLACK_BOT_TOKEN: token,
      SLACK_TEAM_ID: "T_SYNTHETIC",
      SLACK_CHANNEL_IDS: "",
      SLACK_TEST_HEADERS: recorded,
    },
    stdio: ["pipe", "pipe", "pipe"],
  });
  t.after(() => server.kill());
  let errors = "";
  server.stderr.setEncoding("utf8").on("data", (chunk) => { errors += chunk; });
  const pending = new Map();
  const lines = createInterface({ input: server.stdout });
  t.after(() => lines.close());
  lines.on("line", (line) => {
    const message = JSON.parse(line);
    const entry = pending.get(message.id);
    if (entry) {
      pending.delete(message.id);
      if (message.error) entry.reject(new Error(JSON.stringify(message.error)));
      else entry.resolve(message.result);
    }
  });
  server.on("exit", () => {
    for (const entry of pending.values()) entry.reject(new Error(errors));
  });
  let id = 0;
  function request(method, params = {}) {
    id += 1;
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject });
      server.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    });
  }
  await request("initialize", {
    protocolVersion: "2024-11-05", capabilities: {},
    clientInfo: { name: "offline-auth-regression", version: "1" },
  });
  server.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
  const tools = await request("tools/list");
  assert.deepEqual(tools.tools.map((tool) => tool.name).sort(), [
    "slack_add_reaction", "slack_get_channel_history", "slack_get_thread_replies",
    "slack_get_user_profile", "slack_get_users", "slack_list_channels",
    "slack_post_message", "slack_reply_to_thread",
  ]);
  const result = await request("tools/call", { name: "slack_list_channels", arguments: {} });
  assert.deepEqual(JSON.parse(result.content[0].text), { ok: true, channels: [] });
  const headers = new Headers(JSON.parse(await readFile(recorded, "utf8")));
  assert.equal(headers.get("authorization"), `Bearer ${token}`);
  assert.equal(headers.get("cookie"), `d=${cookie}`);
  assert.equal(headers.get("content-type"), "application/json");
});
