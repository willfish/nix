import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import test from "node:test";
const binary = process.env.LOCK_POLICY_BIN!,
  legacy = process.env.LOCK_POLICY_LEGACY,
  python = process.env.LOCK_POLICY_PYTHON || "python3";
assert.ok(binary, "Set LOCK_POLICY_BIN");
function github(owner = "willfish", repo = "fixture") {
  return {
    locked: {
      type: "github",
      owner,
      repo,
      rev: "a".repeat(40),
      narHash: "sha256-" + "A".repeat(43) + "=",
      lastModified: 1,
    },
    original: { type: "github", owner, repo },
  };
}
function base(): any {
  return {
    version: 7,
    root: "root",
    nodes: {
      root: { inputs: { app: "app" } },
      app: { ...github(), inputs: { dep: "dep" } },
      dep: github("NixOS", "nixpkgs"),
    },
  };
}
function check(before: any, after: any, status: number, owners = "willfish\n") {
  const root = mkdtempSync(join(tmpdir(), "lock-policy-"));
  try {
    const paths = ["base", "head"].map((n) => join(root, n));
    for (const [i, v] of [before, after].entries())
      writeFileSync(paths[i], typeof v === "string" ? v : JSON.stringify(v));
    let output = "";
    for (const old of legacy ? [false, true] : [false]) {
      const r = spawnSync(
        old ? python : binary,
        old ? [legacy!, ...paths] : paths,
        {
          encoding: "utf8",
          timeout: 10000,
          env: { ...process.env, AUTO_MERGE_GITHUB_OWNERS: owners },
        },
      );
      assert.ifError(r.error);
      assert.equal(r.status, status, r.stderr);
      if (status) assert.match(r.stderr, /Manual review required:/);
      else if (old) assert.equal(r.stdout, output);
      else output = r.stdout;
      assert.doesNotMatch(
        r.stderr,
        /AddressSanitizer|LeakSanitizer|runtime error:|DEADLYSIGNAL/,
      );
    }
    return output;
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

test("unchanged valid graphs and approved direct/transitive revisions are accepted and sorted", () => {
  const a = base();
  check(a, a, 0);
  let b = structuredClone(a);
  b.nodes.app.locked.rev = "b".repeat(40);
  assert.match(check(a, b, 0), /app/);
  a.nodes.dep = github("willfish", "library");
  b = structuredClone(a);
  for (const name of ["app", "dep"])
    b.nodes[name].locked = {
      ...b.nodes[name].locked,
      rev: "c".repeat(40),
      narHash: "sha256-" + "B".repeat(42) + "A=",
      lastModified: 2,
    };
  assert.equal(
    check(a, b, 0),
    "Auto-merge allowed for changed lock node(s): app, dep\n",
  );
});
test("allowlisting is exact, has no default and includes disconnected and mixed-owner changes", () => {
  const a = base();
  let b = structuredClone(a);
  b.nodes.app.locked.rev = "b".repeat(40);
  for (const owners of [
    "",
    "Willfish",
    "willfish-other",
    " willfish",
    "willfish ",
    "willfish\t",
  ])
    check(a, b, 1, owners);
  check(a, b, 0, "other\vwillfish\u0085last");
  b.nodes.dep.locked.rev = "b".repeat(40);
  check(a, b, 1);
  a.nodes.app.inputs = {};
  b = structuredClone(a);
  b.nodes.dep.locked.rev = "b".repeat(40);
  check(a, b, 1);
});
test("root identity, node sets, input graphs, follows spelling and original metadata cannot change", () => {
  for (const change of [
    (b: any) => (b.nodes.extra = github()),
    (b: any) => delete b.nodes.dep,
    (b: any) => (b.nodes.root.inputs.app = "dep"),
    (b: any) => (b.nodes.app.inputs = {}),
    (b: any) => (b.nodes.app.inputs.extra = "dep"),
    (b: any) => (b.nodes.app.original.ref = "branch"),
    (b: any) => (b.nodes.dep.flake = false),
    (b: any) => {
      b.nodes.n1 = b.nodes.root;
      delete b.nodes.root;
      b.root = "n1";
    },
  ]) {
    const a = base(),
      b = structuredClone(a);
    change(b);
    check(a, b, 1);
  }
  const a = base();
  a.nodes.root.inputs.alias = "dep";
  a.nodes.app.inputs.shared = ["alias"];
  const b = structuredClone(a);
  b.nodes.app.inputs.shared = ["app", "dep"];
  check(a, b, 1);
});
test("locked identity/host/extra attributes and changed non-GitHub sources require review", () => {
  for (const [key, value] of [
    ["owner", "other"],
    ["repo", "other"],
    ["host", "other.example"],
    ["dir", "subdir"],
    ["ref", "branch"],
    ["type", "gitlab"],
  ]) {
    const a = base(),
      b = structuredClone(a);
    b.nodes.app.locked[key] = value;
    check(a, b, 1);
  }
  for (const extra of [
    { host: "other.example" },
    { ref: "main" },
    { revCount: 2 },
    { custom: true },
  ]) {
    const a = base();
    Object.assign(a.nodes.app.locked, extra);
    const b = structuredClone(a);
    b.nodes.app.locked.rev = "b".repeat(40);
    check(a, b, 1);
  }
  const a = base();
  a.nodes.app.locked.type = "gitlab";
  const b = structuredClone(a);
  b.nodes.app.locked.rev = "b".repeat(40);
  check(a, b, 1);
});
test("all supported source types validate unchanged; locked indirect and malformed attributes fail closed", () => {
  for (const type of [
    "github",
    "gitlab",
    "sourcehut",
    "git",
    "mercurial",
    "tarball",
    "file",
    "path",
    "indirect",
  ]) {
    const a = base();
    a.nodes.app.inputs = {};
    a.nodes.app.original = {
      type,
      ...(["github", "gitlab", "sourcehut"].includes(type)
        ? { owner: "vendor", repo: "repo" }
        : type === "path"
          ? { path: "/source" }
          : type === "indirect"
            ? { id: "nixpkgs" }
            : { url: "https://example.test/source" }),
    };
    if (type !== "indirect")
      a.nodes.app.locked = {
        ...a.nodes.app.original,
        narHash: "sha256-" + "A".repeat(43) + "=",
        ...(type === "github" ? { rev: "a".repeat(40) } : {}),
      };
    check(a, a, 0);
  }
  for (const [field, value] of [
    ["type", "future"],
    ["type", "indirect"],
    ["narHash", "sha256-???"],
    ["narHash", "sha512-" + "A".repeat(43) + "="],
    ["rev", "A".repeat(40)],
    ["rev", null],
    ["owner", ""],
    ["repo", "../repo"],
    ["lastModified", true],
    ["lastModified", -1],
    ["lastModified", 1.5],
    ["revCount", -1],
    ["dir", true],
    ["ref", 1],
    ["custom", {}],
  ]) {
    const a = base();
    a.nodes.app.locked[field] = value;
    check(a, a, 1);
  }
});
test("malformed roots, node flags, duplicate keys and invalid JSON are rejected in either input", () => {
  for (const lock of [
    null,
    [],
    {},
    { ...base(), version: true },
    { ...base(), version: 8 },
    { ...base(), root: "missing" },
    { ...base(), nodes: [] },
    { ...base(), extra: true },
  ])
    check(lock, lock, 1);
  for (const node of [
    null,
    {},
    { locked: null },
    { extra: 1 },
    { ...github(), inputs: [] },
    { ...github(), flake: 0 },
    { ...github(), flake: false, inputs: { dep: "dep" } },
  ]) {
    const a = base();
    a.nodes.app = node;
    check(a, a, 1);
  }
  const a = base(),
    duplicate = JSON.stringify(a).replace(
      '"version":7',
      '"version":7,"version":7',
    );
  check(a, duplicate, 1);
  check(duplicate, a, 1);
  check(
    a,
    JSON.stringify(a).replace(
      '"owner":"willfish"',
      '"owner":"willfish","owner":"willfish"',
    ),
    1,
  );
  check(a, "not JSON", 1);
});
test("nested follows and empty root aliases resolve; ordinary cycles stay legal but alias cycles fail", () => {
  const a = base();
  a.nodes.root.inputs.alias = ["app", "dep"];
  a.nodes.root.inputs.second = ["alias"];
  a.nodes.app.inputs.shared = ["second"];
  a.nodes.app.inputs.parent = [];
  check(a, a, 0);
  const direct = base();
  direct.nodes.dep.inputs = { back: "app" };
  check(direct, direct, 0);
  for (const aliases of [
    { loop: ["loop"] },
    { x: ["y"], y: ["x"] },
    { x: ["app", "shared"] },
  ]) {
    const b = base();
    Object.assign(b.nodes.root.inputs, aliases);
    b.nodes.app.inputs.shared = [Object.keys(aliases)[0]];
    check(b, b, 1);
  }
});
test("dangling and malformed references are checked even when unreachable from the root", () => {
  for (const target of [
    "absent",
    ["absent"],
    ["app", "missing"],
    [1],
    null,
    1,
    true,
    { follows: "app" },
    [""],
    ["bad\nname"],
    "dep\0suffix",
  ]) {
    const a = base();
    a.nodes.root.inputs = {};
    a.nodes.app.inputs.dep = target;
    check(a, a, 1);
  }
});
test("source integer/bool types stay distinct and metadata key ordering does not matter", () => {
  const a = base();
  a.nodes.app.original.custom = true;
  const b = structuredClone(a);
  b.nodes.app.original.custom = 1;
  check(a, b, 1);
  const c = base();
  const d = JSON.parse(JSON.stringify(c));
  d.nodes.app.original = { repo: "fixture", owner: "willfish", type: "github" };
  check(c, d, 0);
  check(
    JSON.stringify(c).replace('"lastModified":1', '"lastModified":-0'),
    JSON.stringify(c).replace('"lastModified":1', '"lastModified":0'),
    0,
  );
});
test("current repository lock and a single allowed owner revision work without evaluating inputs", () => {
  const a = JSON.parse(
      readFileSync(new URL("../../../flake.lock", import.meta.url), "utf8"),
    ),
    b = structuredClone(a);
  check(a, b, 0);
  const entry: any = Object.values(b.nodes).find(
    (n: any) => n.locked?.type === "github" && n.locked.owner === "willfish",
  );
  assert.ok(entry);
  entry.locked.rev = "b".repeat(40);
  check(a, b, 0);
});
