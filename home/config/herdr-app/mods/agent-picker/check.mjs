// Offline contract checks. No Herdr commands or filesystem mutations are made.
// Run: node home/config/herdr-app/mods/agent-picker/check.mjs
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { agentRows, fuzzy } from "./model.js";
import { spaceRows, homeCandidate, folderKey, folderName } from "../workspacex/model.js";

const space = { id: "s1", name: "Tariff", folder: "/home/test/tariff", branch: "main", machine: null, active: true };
const agent = { address: "t23", space: "s1", space_name: "Tariff", tab_name: "API", title: "Fix login", status: "working", program: { name: "OpenCode" } };
const terminal = { address: "t23", cwd: "/home/test/tariff/api", labels: { "user.agent.display": "opencode · high · gpt-6.1-sol" } };
for (const query of ["opc", "high sol", "g61s", "fxlgn", "trf", "api", "working"]) {
  assert.equal(agentRows([agent], [terminal], [space], query)[0]?.key, "t23", query);
}
assert.equal(agentRows([agent], [terminal], [space], "sol")[0].display, terminal.labels["user.agent.display"]);
assert.equal(fuzzy("zzz", "opencode"), null);
assert(fuzzy("api", "api") > fuzzy("api", "a long path inside"));
const other = { ...space, id: "remote/s2", active: false, machine: "remote", branch: "feature-login" };
assert.deepEqual(spaceRows([space, other], "", ["/new"]).map(r => r.id), [other.id]);
assert.equal(spaceRows([space, other], "ftlgn remote", [])[0]?.id, other.id);
assert.equal(spaceRows([space], "tariff", [space.folder]).length, 0);
assert.equal(spaceRows([], "tariff", [space.folder, `${space.folder}/`]).length, 1);
assert.equal(spaceRows([other], "tariff", [space.folder]).filter(r => r.kind === "create").length, 1);
assert.equal(homeCandidate("/home/test", "Repos/folder with spaces"), "/home/test/Repos/folder with spaces");
for (const query of ["", "/tmp", "../tmp", "x/../tmp", "x//y", "x\n"]) assert.equal(homeCandidate("/home/test", query), null);

// Execute only action methods with host functions replaced by offline fixtures.
async function load(relative, bindings) {
  const source = (await readFile(new URL(relative, import.meta.url), "utf8"))
    .replace(/^import .*;\n/gm, "").replace("export default class", "return class");
  return new Function(...Object.keys(bindings), source)(...Object.values(bindings));
}
const calls = [];
const host = {
  View: class {}, props: () => ({ overlay: "picker" }), closeOverlay: id => calls.push(["close", id]),
  run: (command, args) => calls.push([command, args]),
};
const cx = { notify() {}, spawn(task) { this.pending = task(this); } };
let liveAgents = [agent];
const AgentPicker = await load("./main.js", { ...host, agents: () => liveAgents });
const picker = new AgentPicker();
picker.go({ key: "t23" }, cx);
assert.deepEqual(calls.shift(), ["agent.focus", { terminal: "t23" }]);
assert.deepEqual(calls.shift(), ["close", "picker"]);
liveAgents = [];
picker.go({ key: "t23" }, cx);
assert.match(picker.error, /no longer available/);
assert.equal(calls.length, 0);

let liveSpaces = [];
let missing = false;
const commands = [];
const exec = async (command, args) => {
  commands.push([command, args]);
  if (command === "zoxide") return { code: 0, stdout: "", stderr: "" };
  if (missing) return { code: 1, stdout: "", stderr: "not found" };
  return { code: 0, stdout: command === "realpath" ? "/home/test/tariff\n" : "", stderr: "" };
};
const Workspacex = await load("../workspacex/main.js", {
  ...host, spaces: () => liveSpaces, machines: () => [{ name: null, layout: "ready" }],
  home: () => "/home/test", exec, spaceRows, folderKey, folderName, homeCandidate,
});
const workspace = new Workspacex();
workspace.busy = false;
const create = { kind: "create", folder: "/home/test/tariff" };
workspace.go(create, cx);
await cx.pending;
assert.deepEqual(calls.shift(), ["space.new", { name: "tariff", folder: "/home/test/tariff", machine: "" }]);
assert.deepEqual(calls.shift(), ["close", "picker"]);
liveSpaces = [{ ...space, folder: "/home/test/alias" }];
workspace.closed = false;
workspace.go(create, cx);
await cx.pending;
assert.deepEqual(calls.shift(), ["space.select", { id: space.id }]);
assert.deepEqual(calls.shift(), ["close", "picker"]);
missing = true;
workspace.closed = false;
workspace.go(create, cx);
await cx.pending;
assert.match(workspace.error, /no longer available/);
assert.equal(calls.length, 0);
assert.equal(workspace.busy, false);
missing = false;
workspace.go(create, cx);
workspace.dismiss();
await cx.pending;
assert.deepEqual(calls.shift(), ["close", "picker"]);
assert.equal(calls.length, 0, "closing during folder validation must not create a space");
workspace.closed = false;
workspace.query = "$(touch injected); 'quoted'";
workspace.generation = 0;
workspace.lookup(cx);
await cx.pending;
assert.deepEqual(commands.find(c => c[0] === "zoxide"), ["zoxide", ["query", "--list", "--", "$(touch", "injected);", "'quoted'"]]);
assert(commands.every(([command]) => ["zoxide", "realpath", "test"].includes(command)));
missing = false;
workspace.query = "tariff";
workspace.lookup(cx);
await cx.pending;
assert.deepEqual(workspace.folders, ["/home/test/tariff"]);
console.log("Picker matcher and action checks passed (offline fixtures).");
