// Run manually against the built or deployed todo.ts with Node 24 or newer:
// node --disable-warning=ExperimentalWarning todo-render.test.mjs <todo.ts>
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { stripTypeScriptTypes } from "node:module";
import test from "node:test";
import { runInNewContext } from "node:vm";

const sourcePath = process.argv[2];
assert.ok(sourcePath, "Pass the built or deployed todo.ts path");
const source = readFileSync(sourcePath, "utf8")
  .replace(/^import .*;\n/gm, "")
  .replace("export default function", "function register");

// Exercise the actual extension without loading the interactive Pi runtime.
function load() {
  let tool;
  const events = {};
  const commands = {};
  class Text {
    constructor(text) { this.text = text; }
    render() { return [this.text]; }
  }
  const Type = {
    Object: value => value,
    Optional: value => value,
    String: () => ({}),
    Number: () => ({}),
  };
  runInNewContext(`${stripTypeScriptTypes(source)}\nregister(pi);`, {
    Type,
    StringEnum: value => value,
    Text,
    pi: {
      on(name, handler) { events[name] = handler; },
      registerTool(value) { tool = value; },
      registerCommand(name, value) { commands[name] = value; },
    },
  });
  assert.equal(tool.name, "todo");
  return { tool, events, commands };
}

const theme = { fg: (_color, text) => text, bold: text => text };
function render(tool, result, expanded = false) {
  const component = tool.renderResult(result, { expanded }, theme, {});
  assert.ok(component && typeof component.render === "function", "Renderer must return a component");
  return component.render(80).join("\n");
}

for (const expanded of [false, true]) {
  test(`validation errors render safely, expanded=${expanded}`, () => {
    const { tool } = load();
    const message = 'Validation failed for tool "todo": action must be an allowed value';
    for (const details of [undefined, {}, { action: "text" }]) {
      assert.equal(render(tool, {
        content: [{ type: "text", text: message }], details, isError: true,
      }, expanded), message);
    }
    assert.equal(render(tool, { content: [], details: {} }, expanded), "");
  });
}

test("malformed call display never throws", () => {
  const { tool } = load();
  for (const args of [{}, { action: "text", id: 0, text: "placeholder" }]) {
    assert.ok(tool.renderCall(args, theme, {}).render(80));
  }
});

test("all todo actions and tool errors retain working renderers", async () => {
  const { tool } = load();
  const execute = params => tool.execute("test", params);
  assert.match(render(tool, await execute({ action: "list" })), /No todos/);
  assert.match(render(tool, await execute({ action: "add" })), /text required/);
  assert.match(render(tool, await execute({ action: "add", text: "first" })), /#1 first/);
  assert.match(render(tool, await execute({ action: "toggle" })), /id required/);
  assert.match(render(tool, await execute({ action: "toggle", id: 0 })), /#0 not found/);
  assert.match(render(tool, await execute({ action: "toggle", id: 1 })), /completed/);
  assert.match(render(tool, await execute({ action: "toggle", id: 1 })), /uncompleted/);
  for (let i = 2; i <= 7; i++) await execute({ action: "add", text: `item ${i}` });
  const list = await execute({ action: "list" });
  assert.match(render(tool, list), /2 more/);
  assert.doesNotMatch(render(tool, list), /#7/);
  assert.match(render(tool, list, true), /#7 item 7/);
  assert.match(render(tool, await execute({ action: "text" })), /unknown action/);
  assert.match(render(tool, await execute({ action: "clear" })), /Cleared all todos/);
  assert.match(render(tool, await execute({ action: "list" })), /No todos/);
  assert.match(render(tool, await execute({ action: "add", text: "reset" })), /#1 reset/);
});

test("restoring a session skips validation errors and restores todo state", async () => {
  const { tool, events } = load();
  const restored = {
    action: "add", todos: [{ id: 3, text: "restored", done: false }], nextId: 4,
  };
  // Validation failures have no state snapshot and must not replace it.
  const ctx = { sessionManager: { getBranch: () => [
    { type: "message", message: { role: "toolResult", toolName: "todo", details: restored } },
    { type: "message", message: { role: "toolResult", toolName: "todo", details: {} } },
  ] } };
  for (const name of ["session_start", "session_tree"]) {
    await events[name]({}, ctx);
    assert.match(render(tool, await tool.execute("test", { action: "list" })), /#3 restored/);
    assert.match(render(tool, await tool.execute("test", { action: "add", text: "next" })), /#4 next/);
  }
});
