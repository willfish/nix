import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { after, test } from "node:test";
const root = fs.realpathSync(fs.mkdtempSync(join(tmpdir(), "token-report-")));
after(() => {
  if (!process.env.KEEP_TOKEN_FIXTURE)
    fs.rmSync(root, { recursive: true, force: true });
});
let serial = 0;
const binary =
  process.env.TOKEN_REPORT_BIN ||
  join(process.env.SKILL_TOOLS_BIN!, "pi-token-report");
function fixture() {
  const home = join(root, String(serial++)),
    state = join(home, "state");
  fs.mkdirSync(join(state, "prompt-capture"), { recursive: true });
  return {
    home,
    state,
    env: { ...process.env, HOME: home, XDG_STATE_HOME: state },
  };
}
function capture(f: ReturnType<typeof fixture>, rows: any[]) {
  fs.writeFileSync(
    join(f.state, "prompt-capture/pi.jsonl"),
    rows.map((row) => JSON.stringify(row)).join("\n") +
      "\nmalformed ignored line\n",
  );
}
function rows(run = "fixture") {
  const tools = [
    {
      type: "function",
      name: "skill_catalog",
      description: "find skills",
      parameters: { type: "object" },
    },
    {
      type: "function",
      name: "mcp",
      description: "mcp status",
      parameters: { type: "object" },
    },
  ];
  const first = {
    model: "grok-4.6",
    tools,
    input: [
      { role: "developer", content: "PRIVATE_INSTRUCTIONS" },
      { role: "user", content: "turn 1" },
    ],
  };
  const second = {
    ...first,
    input: [
      ...first.input,
      {
        type: "function_call",
        name: "skill_catalog",
        call_id: "call-1",
        arguments: JSON.stringify({ query: "local development nix" }),
      },
      {
        type: "function_call_output",
        call_id: "call-1",
        output: "PRIVATE_TOOL_RESULT",
      },
    ],
  };
  return [
    {
      run,
      kind: "request",
      flow_id: "f1",
      ts: "t1",
      url: "api.x.ai/v1/responses",
      request_body: JSON.stringify(first),
      request_chars: JSON.stringify(first).length,
    },
    {
      run,
      kind: "usage",
      flow_id: "f1",
      usage: { input_tokens: 1000, output_tokens: 20, total_tokens: 1020 },
    },
    { run, kind: "response", flow_id: "f1", status: 200 },
    {
      run,
      kind: "request",
      flow_id: "f2",
      ts: "t2",
      url: "api.x.ai/v1/responses",
      request_body: JSON.stringify(second),
      request_chars: JSON.stringify(second).length,
    },
    {
      run,
      kind: "usage",
      flow_id: "f2",
      usage: {
        input_tokens: 1100,
        output_tokens: 10,
        cached_tokens: 800,
        total_tokens: 1110,
        output_tokens_details: { reasoning_tokens: 4 },
      },
    },
    { run, kind: "response", flow_id: "f2", status: 200 },
  ];
}
function report(f: ReturnType<typeof fixture>, run = "fixture") {
  const result = spawnSync(binary, ["--from-run", run], {
    env: f.env,
    encoding: "utf8",
    timeout: 10000,
  });
  assert.equal(result.status, 0, result.stderr);
  const htmlPath = join(f.state, "prompt-capture/reports", run + ".html");
  return {
    data: JSON.parse(
      fs.readFileSync(htmlPath.replace(/\.html$/, ".json"), "utf8"),
    ),
    html: fs.readFileSync(htmlPath, "utf8"),
    htmlPath,
  };
}
test("Responses requests, cache-aware costs, distinct calls and private report publication", () => {
  const f = fixture();
  capture(f, rows());
  const r = report(f);
  assert.equal(r.data.meta.request_count, 2);
  assert.equal(r.data.meta.record_count, 6);
  assert.deepEqual(r.data.requests[0].tool_calls, []);
  assert.deepEqual(r.data.requests[1].skill_queries, ["local development nix"]);
  assert.equal(
    r.data.requests[1].result_chars.skill_catalog,
    "PRIVATE_TOOL_RESULT".length,
  );
  assert.equal(r.data.requests[1].usage.reasoning, 4);
  assert.equal(r.data.attribution.tools.skill_catalog.unique_calls, 1);
  assert.equal(r.data.attribution.tools.skill_catalog.wire_appearances, 1);
  assert(Math.abs(r.data.requests[1].cost_usd - 0.00106) < 1e-12);
  assert(r.html.includes('aria-label="Tokens per LLM request"'));
  for (const secret of [
    "PRIVATE_INSTRUCTIONS",
    "PRIVATE_TOOL_RESULT",
    "request_body",
  ])
    assert(
      !r.html.includes(secret) && !JSON.stringify(r.data).includes(secret),
    );
  assert.equal(fs.statSync(r.htmlPath).mode & 0o777, 0o600);
  assert.equal(
    fs.statSync(r.htmlPath.replace(".html", ".json")).mode & 0o777,
    0o600,
  );
  assert.equal(fs.statSync(dirname(r.htmlPath)).mode & 0o777, 0o700);
  assert.equal(
    fs.readlinkSync(join(dirname(r.htmlPath), "latest.html")),
    "fixture.html",
  );
  if (process.env.KEEP_TOKEN_FIXTURE) console.log("TOKEN_HTML=" + r.htmlPath);
});
test("streaming usage takes maxima, aliases and Chat calls retain their counting rules", () => {
  const f = fixture();
  const input = {
    model: "grok-4.6",
    functions: [{ function: { name: "bash" } }],
    messages: [
      {
        role: "assistant",
        tool_calls: [
          {
            id: "chat-1",
            function: { name: "bash", arguments: '{"command":"private"}' },
          },
        ],
      },
    ],
  };
  capture(f, [
    {
      run: "fixture",
      kind: "request",
      flow_id: "f",
      url: "https://service/chat/completions",
      request_body: JSON.stringify(input),
    },
    {
      run: "fixture",
      kind: "usage",
      flow_id: "f",
      usage: { input_tokens: 100, output_tokens: 1 },
    },
    {
      run: "fixture",
      kind: "usage",
      flow_id: "f",
      usage: {
        promptTokens: 100,
        completionTokens: 8,
        prompt_tokens_details: { cachedTokens: 40 },
      },
    },
    {
      run: "fixture",
      kind: "request",
      flow_id: "asset",
      url: "https://example/asset",
      request_body: "{}",
    },
    {
      run: "other",
      kind: "usage",
      flow_id: "other",
      usage: { input_tokens: 999 },
    },
  ]);
  const r = report(f).data;
  assert.equal(r.requests.length, 1);
  assert.deepEqual(r.requests[0].usage, {
    input: 100,
    output: 8,
    total: 108,
    cached: 40,
  });
  assert(Math.abs(r.requests[0].cost_usd - 0.000188) < 1e-12);
  assert.equal(r.attribution.tools.bash.unique_calls, 0);
  assert.equal(r.attribution.tools.bash.wire_appearances, 1);
});
test("custom prices and privacy guard keep raw arguments, URL credentials and bodies out of both artifacts", () => {
  const f = fixture();
  fs.mkdirSync(join(f.home, ".pi/agent"), { recursive: true });
  fs.writeFileSync(
    join(f.home, ".pi/agent/models-store.json"),
    JSON.stringify({
      xai: {
        models: [
          { id: "grok-4.6", cost: { input: 3, output: 9, cacheRead: 1 } },
        ],
      },
    }),
  );
  const payload = {
    tools: [{ name: "mcp" }],
    input: [
      {
        type: "function_call",
        name: "mcp",
        call_id: "mcp-1",
        arguments: JSON.stringify({
          tool: "send",
          args: { password: "SECRET_ARGUMENT" },
        }),
      },
      {
        type: "function_call",
        name: "skill_catalog",
        call_id: "skill-1",
        arguments: '{"query":"SECRET_QUERY"}',
      },
      {
        type: "function_call_output",
        call_id: "mcp-1",
        output: "SECRET_RESULT",
      },
    ],
  };
  capture(f, [
    {
      run: "fixture",
      kind: "request",
      flow_id: "f",
      url: "https://user:SECRET_PASSWORD@api.x.ai/v1/responses?key=SECRET_URL",
      request_body: JSON.stringify(payload),
    },
    {
      run: "fixture",
      kind: "usage",
      flow_id: "f",
      usage: { input_tokens: 100, output_tokens: 2 },
    },
  ]);
  const r = report(f);
  assert.equal(r.data.rates.input, 3);
  assert.equal(r.data.requests[0].url, "https://api.x.ai/v1/responses");
  assert(!r.html.includes("SECRET_"));
  assert(!JSON.stringify(r.data).includes("SECRET_"));
  assert.deepEqual(r.data.requests[0].mcp_calls, ["[arguments omitted]"]);
  assert.deepEqual(r.data.requests[0].skill_queries, ["[query omitted]"]);
});
test("stub capture isolates the parent, keeps the fixed workload and redacts failed probe output", () => {
  const f = fixture(),
    local = join(f.home, ".local/bin");
  fs.mkdirSync(local, { recursive: true });
  fs.writeFileSync(
    join(local, "pi"),
    "exec /nix/store/first/bin/pi\nexec /nix/store/last/bin/pi\n",
  );
  const receipt = join(f.home, "receipt.json");
  const script = `#!${process.execPath}\nconst fs=require('node:fs'),p=require('node:path');const args=process.argv.slice(2);const runner=args[2];const source=fs.readFileSync(runner,'utf8');const keys=['PI_SESSION_FILE','PI_SESSION_ID','CAPTURE_PROMPTS','HTTP_PROXY','HTTPS_PROXY','http_proxy','https_proxy','SSL_CERT_FILE','REQUESTS_CA_BUNDLE','NODE_EXTRA_CA_CERTS'];fs.writeFileSync(process.env.RECEIPT,JSON.stringify({args,source,unset:keys.every(k=>process.env[k]===undefined),telemetry:process.env.PI_TELEMETRY}));fs.writeFileSync(p.join(p.dirname(runner),'add.py'),'SENSITIVE_PROBE_VALUE');console.log('run 20260912T104853-77612');process.exit(17);\n`;
  fs.writeFileSync(join(local, "prompt-capture"), script, { mode: 0o755 });
  capture(f, rows("20260912T104853-77612"));
  const result = spawnSync(binary, ["--capture"], {
    env: {
      ...f.env,
      RECEIPT: receipt,
      PI_SESSION_FILE: "parent",
      PI_SESSION_ID: "parent",
      CAPTURE_PROMPTS: "1",
      HTTP_PROXY: "parent",
      HTTPS_PROXY: "parent",
      http_proxy: "parent",
      https_proxy: "parent",
      SSL_CERT_FILE: "parent",
      REQUESTS_CA_BUNDLE: "parent",
      NODE_EXTRA_CA_CERTS: "parent",
    },
    encoding: "utf8",
    timeout: 10000,
  });
  assert.equal(result.status, 0, result.stderr);
  const got = JSON.parse(fs.readFileSync(receipt, "utf8"));
  assert(got.unset);
  assert.equal(got.telemetry, "0");
  assert.deepEqual(got.args.slice(0, 2), ["pi", "--"]);
  assert(got.source.includes("/nix/store/last/bin/pi"));
  assert.equal(got.source.match(/--continue/g).length, 2);
  assert(got.source.includes("python3 add.py"));
  assert(
    got.source.includes("unset PI_SESSION_FILE PI_SESSION_ID CAPTURE_PROMPTS"),
  );
  assert(
    !result.stdout.includes("SENSITIVE_PROBE_VALUE") &&
      !result.stderr.includes("SENSITIVE_PROBE_VALUE"),
  );
  const archive = JSON.parse(
    fs.readFileSync(
      join(f.state, "prompt-capture/reports/20260912T104853-77612.json"),
      "utf8",
    ),
  );
  assert.equal(archive.meta.exit_code, 17);
  assert.equal(archive.meta.task_result, "add.py=yes output=error");
  assert.equal(
    fs.statSync(join(f.state, "prompt-capture/reports/nested-pi.log")).mode &
      0o777,
    0o600,
  );
});
test("baseline stays counts-only, empty captures return two and run identifiers cannot escape reports", () => {
  const baseline = JSON.parse(
    fs.readFileSync(
      resolve(
        "home/config/llm/skills/pi-token-mitm/scripts/baseline-2026-09-12.json",
      ),
      "utf8",
    ),
  );
  assert.equal(baseline.request_count, 7);
  assert.equal(baseline.run_id, "20260912T104853-77612");
  assert(!JSON.stringify(baseline).match(/request_body|Authorization/));
  const f = fixture();
  assert.equal(
    spawnSync(binary, ["--from-run", "empty"], { env: f.env }).status,
    2,
  );
  const empty = fs.readFileSync(
    join(f.state, "prompt-capture/reports/empty.html"),
    "utf8",
  );
  assert(empty.includes("No LLM requests captured."));
  assert.equal(
    spawnSync(binary, ["--from-run", "../escape"], { env: f.env }).status,
    1,
  );
  assert(!fs.existsSync(join(f.state, "prompt-capture/escape.html")));
  assert.equal(spawnSync(binary, ["--unknown"], { env: f.env }).status, 2);
});
