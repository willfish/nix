// Offline checks. Run: node home/config/herdr-app/mods/balance-panes/check.mjs
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { activeTarget, sameTarget, balancePlan, applyPlan, decodeReply, qualify } from "./model.js";

const pane = id => ({ type: "block", id, content: { terminal: `terminal-${id}` } });
const target = { space: "space", tab: "tab", on: null };
const document = root => ({ spaces: [{ id: "space", name: "Keep this name", folder: "/keep", tabs: [{ id: "tab", name: "Keep this tab", root }, { id: "other", root: pane("untouched") }] }] });
const original = document({ type: "column", id: "split", children: [pane("one"), pane("two"), pane("three")] });
const saved = JSON.stringify(original);
const plan = balancePlan(original, target);
assert.deepEqual(plan.updates, [{ id: "one", weight: 1 }, { id: "two", weight: 1 }, { id: "three", weight: 1 }]);
assert.equal(JSON.stringify(original), saved, "planning must not mutate the document or other tabs");
assert.deepEqual(balancePlan(document(pane("single")), target).updates, []);
const mixed = { type: "row", id: "mixed", children: [pane("left"), { type: "column", id: "nested", children: [pane("top"), { type: "block", id: "mod", content: { mod: "viewer", props: { file: "keep.md" } } }] }] };
assert.deepEqual(balancePlan(document(mixed), target).updates, [{ id: "top", weight: 1 }, { id: "mod", weight: 1 }, { id: "left", weight: 1 }, { id: "nested", weight: 2 }]);
assert.throws(() => balancePlan(document({ type: "unknown", id: "new-format" }), target), /unsupported/);
assert.throws(() => balancePlan(original, { ...target, tab: "closed" }), /no longer available/);
const writes = [];
applyPlan(plan, ["three", "one", "two"], update => writes.push(update));
assert.deepEqual(writes, plan.updates);
assert.throws(() => applyPlan(plan, ["one", "two", "new-pane"], () => assert.fail("stale layouts must not resize")), /panes changed/);
const spaces = [{ id: "remote/space", machine: "remote", active: true, tabs: [{ id: "remote/tab", active: true }] }];
const remote = activeTarget(spaces);
assert.deepEqual(remote, { space: "space", tab: "tab", on: "remote" });
assert.equal(balancePlan(original, remote).updates[0].id, "remote/one");
assert(sameTarget(target, { ...target }));
assert(!sameTarget(target, { ...target, on: "remote" }));
assert.throws(() => decodeReply({ code: 0, stdout: "not json" }), /unreadable/);

// Exercise the real async view with host functions replaced by offline fixtures.
const source = (await readFile(new URL("./main.js", import.meta.url), "utf8"))
  .replace(/^import .*;\n/gm, "").replace("export default class", "return class");
let live = [{ id: "space", machine: null, active: true, tabs: [{ id: "tab", active: true }] }];
let overlay;
let failRead = false;
let changeTab = false;
const calls = [];
const host = {
  View: class {}, props: () => ({ overlay }), activeTarget, sameTarget, balancePlan, applyPlan, decodeReply, qualify,
  spaces: () => live, panes: () => plan.paneIds.map(id => ({ pane: id, tab: "tab" })),
  machines: () => [{ name: null, layout: "ready" }],
  exec: async (command, args) => {
    calls.push([command, args]);
    assert.equal(JSON.parse(args[3]).method, "doc.get", "the core document is read-only");
    if (changeTab) live = [];
    if (failRead) return { code: 1, stderr: "Core unavailable" };
    return { code: 0, stdout: JSON.stringify({ id: "balance-panes", result: { version: 1, value: original } }) };
  },
  run: (command, update) => calls.push([command, update]),
  closeOverlay: id => calls.push(["close", id]), notify: message => calls.push(["notify", message]),
};
const Balance = new Function(...Object.keys(host), source)(...Object.values(host));
const cx = { spawn(task) { this.pending = task(this); } };
new Balance().init({}, cx);
assert.deepEqual(calls, [], "checking a mod must not execute anything");
overlay = "worker";
new Balance().init({}, cx); await cx.pending;
assert.deepEqual(calls.filter(c => c[0] === "layout.resize").map(c => c[1]), plan.updates);
assert.deepEqual(calls.at(-1), ["close", "worker"]);
calls.length = 0;
failRead = true;
new Balance().init({}, cx); await cx.pending;
assert.equal(calls.find(c => c[0] === "notify")[1].level, "error");
assert(!calls.some(c => c[0] === "layout.resize"));
calls.length = 0;
failRead = false; changeTab = true;
new Balance().init({}, cx); await cx.pending;
assert(!calls.some(c => c[0] === "layout.resize"), "switching tabs during the read must cancel the action");
console.log("Pane balance checks passed: proportional weights, read-only planning, stable IDs, stale-pane cancellation, remote routing, error feedback, and safe checking.");
