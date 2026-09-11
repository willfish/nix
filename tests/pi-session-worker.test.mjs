import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import guard from "../home/config/llm/scripts/knowledge_base/sessions_worker_guard.mjs";

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "pi-session-worker-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const queue = path.join(root, ".kb/sessions-queue");
  fs.mkdirSync(queue, { recursive: true });
  const jobsPath = path.join(queue, "jobs.json");
  const output = path.join(queue, "output.md");
  fs.writeFileSync(jobsPath, JSON.stringify([{
    input_path: ".kb/sessions-queue/draft.md", output_path: ".kb/sessions-queue/output.md",
  }]));
  const environment = { ...process.env };
  t.after(() => { process.env = environment; });
  Object.assign(process.env, {
    SESSIONS_KB_ROOT: root, SESSIONS_WORKER_JOBS: jobsPath,
    SESSIONS_WORKER_RESULTS: path.join(queue, "results.json"),
    SESSIONS_WORKER_LOG: path.join(queue, "worker.log"), SESSIONS_MAX_TURNS: "2",
  });
  const events = new Map();
  guard({ on: (name, handler) => events.set(name, handler) });
  const call = (toolName, file) => events.get("tool_call")({ toolName, input: { path: file } });
  return { root, queue, output, call, events };
}

test("worker can read frozen inputs and curated docs and write only assigned artifacts", (t) => {
  const f = fixture(t);
  assert.equal(f.call("read", ".kb/sessions-queue/draft.md"), undefined);
  assert.equal(f.call("read", "sessions/pi/earlier.md"), undefined);
  assert.equal(f.call("write", f.output), undefined);
  assert.equal(f.call("edit", f.output), undefined);
  for (const file of ["sessions/pi/earlier.md", ".kb/sessions-phase-b-index.json", ".kb/other-results.json"]) {
    assert.equal(f.call("write", file).block, true);
  }
});

test("worker cannot read raw transcripts, credentials, external paths or run non-file tools", (t) => {
  const f = fixture(t);
  for (const file of [".raw/sessions/pi/old.md", ".kb/sessions-queue/pi-home/auth.json", "../secret", "/etc/passwd"]) {
    assert.equal(f.call("read", file).block, true);
  }
  for (const name of ["bash", "mcp", "subagent", "unknown"]) {
    assert.equal(f.call(name, f.output).block, true);
  }
  assert.equal(f.call("read", undefined).block, true);
});

test("symlinks and broken links cannot bypass worker read/write boundaries", (t) => {
  const f = fixture(t);
  fs.mkdirSync(path.join(f.root, "sessions"));
  fs.symlinkSync("/etc", path.join(f.root, "sessions/escape"));
  assert.equal(f.call("read", "sessions/escape/passwd").block, true);
  fs.symlinkSync(path.join(f.root, "missing"), f.output);
  assert.equal(f.call("write", f.output).block, true);
});

test("worker stops further tool batches after the configured turn budget", (t) => {
  const f = fixture(t);
  f.events.get("turn_start")();
  assert.equal(f.call("write", f.output), undefined);
  f.events.get("turn_start")();
  assert.deepEqual(f.call("write", f.output), {
    block: true, terminate: true, reason: "Session worker turn limit reached",
  });
});
