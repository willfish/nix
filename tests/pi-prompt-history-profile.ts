// Copied into the pinned upstream __tests__ directory by the Nix package.
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { getAgentDir } from "@earendil-works/pi-coding-agent";
import { resolvePromptHistoryConfig } from "../src/config";
import { resolveConfigForContext } from "../src/commands";

function fixture(run: (root: string) => void): void {
  const root = mkdtempSync(join(tmpdir(), "pi-history-profiles-"));
  try { run(root); } finally { rmSync(root, { recursive: true, force: true }); }
}

function json(path: string, value: unknown): void {
  writeFileSync(path, JSON.stringify(value));
}

test("default history paths follow the active Pi profile, including bundled defaults", () => {
  const config = resolvePromptHistoryConfig({ cwd: tmpdir() });
  assert.equal(config.sessionDir, join(getAgentDir(), "sessions"));
  assert.equal(config.dbPath, join(getAgentDir(), "prompt-history", "history.db"));
});

test("standard and Qwen profiles have separate indexes, session roots and settings", () => fixture(root => {
  const extensionDir = join(root, "extension");
  const cwd = join(root, "project");
  const standard = join(root, "home", ".pi", "agent");
  const qwen = join(root, "home", ".config", "local-llm", "pi");
  for (const dir of [extensionDir, cwd, join(standard, "extensions"), join(qwen, "extensions")]) {
    mkdirSync(dir, { recursive: true });
  }
  json(join(standard, "extensions", "prompt-history.json"), { maxResults: 31 });
  json(join(qwen, "extensions", "prompt-history.json"), { maxResults: 47 });
  for (const [agentDir, maxResults] of [[standard, 31], [qwen, 47]] as const) {
    const config = resolvePromptHistoryConfig({ extensionDir, cwd, agentDir });
    assert.equal(config.sessionDir, join(agentDir, "sessions"));
    assert.equal(config.dbPath, join(agentDir, "prompt-history", "history.db"));
    assert.equal(config.maxResults, maxResults);
    assert.equal(config.primaryAction, "copy");
  }
}));

test("explicit paths and project overrides retain precedence in an isolated profile", () => fixture(root => {
  const extensionDir = join(root, "extension");
  const agentDir = join(root, "qwen");
  const cwd = join(root, "project");
  for (const dir of [extensionDir, join(agentDir, "extensions"), join(cwd, ".pi", "extensions")]) {
    mkdirSync(dir, { recursive: true });
  }
  json(join(agentDir, "extensions", "prompt-history.json"), {
    dbPath: join(root, "custom.db"), sessionDir: join(root, "custom-sessions"), maxResults: 30,
  });
  json(join(cwd, ".pi", "extensions", "prompt-history.json"), { maxResults: 40 });
  const config = resolvePromptHistoryConfig({ extensionDir, agentDir, cwd, projectTrusted: true });
  assert.equal(config.dbPath, join(root, "custom.db"));
  assert.equal(config.sessionDir, join(root, "custom-sessions"));
  assert.equal(config.maxResults, 40);
}));

test("untrusted project config cannot redirect the index or session root", () => fixture(root => {
  const cwd = join(root, "untrusted");
  mkdirSync(join(cwd, ".pi", "extensions"), { recursive: true });
  json(join(cwd, ".pi", "extensions", "prompt-history.json"), {
    dbPath: join(cwd, "stolen.db"), sessionDir: join(root, "other-profile"),
  });
  for (const ctx of [{ cwd }, { cwd, isProjectTrusted: () => false }]) {
    const config = resolveConfigForContext(ctx);
    assert.equal(config.dbPath, join(getAgentDir(), "prompt-history", "history.db"));
    assert.equal(config.sessionDir, join(getAgentDir(), "sessions"));
  }
  const trusted = resolveConfigForContext({ cwd, isProjectTrusted: () => true });
  assert.equal(trusted.dbPath, join(cwd, "stolen.db"));
  assert.equal(trusted.sessionDir, join(root, "other-profile"));
}));
