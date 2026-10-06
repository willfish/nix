import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { after, test } from "node:test";
const bin = process.env.SKILL_AUDIT_BIN!;
assert(bin, "set SKILL_AUDIT_BIN");
const temporary = fs.realpathSync(
  fs.mkdtempSync(join(tmpdir(), "skill-audit-")),
);
after(() => fs.rmSync(temporary, { recursive: true, force: true }));
let serial = 0;
function write(path: string, text: string) {
  fs.mkdirSync(dirname(path), { recursive: true });
  fs.writeFileSync(path, text);
}
function fixture() {
  const root = join(temporary, String(serial++)),
    llm = join(root, "home/config/llm");
  const entry: any = {
    name: "example",
    kind: "shared",
    invocation: "reference-backed domain skill",
    hermes: false,
    references: { "example.md": "guides/example.md" },
  };
  write(
    join(llm, "skills/example/SKILL.md"),
    "---\nname: example\ndescription: A fixture skill\n---\nRead references/example.md\n",
  );
  write(join(llm, "guides/example.md"), "Reference\n");
  const catalog = join(llm, "skill-catalog.json");
  write(catalog, JSON.stringify([entry]));
  return { root, llm, entry, catalog };
}
function audit(root: string, parity = true) {
  const result = spawnSync(bin, ["--root", root], {
    encoding: "utf8",
    timeout: 10000,
  });
  assert.notEqual(result.status, null, result.stderr);
  assert(!result.stderr.includes("Sanitizer"), result.stderr);
  if (parity && process.env.SKILL_AUDIT_REFERENCE) {
    const old = spawnSync(process.env.SKILL_AUDIT_REFERENCE, ["--root", root], {
      encoding: "utf8",
      timeout: 10000,
    });
    assert.equal(
      result.status,
      old.status,
      `${result.stdout}\n${result.stderr}\n${old.stdout}\n${old.stderr}`,
    );
    if (!result.stderr.startsWith("Invalid skill catalogue:"))
      assert.equal(result.stdout, old.stdout);
  }
  return result;
}
function reject(value: any) {
  const f = fixture();
  write(f.catalog, JSON.stringify(value));
  const r = audit(f.root);
  assert.equal(r.status, 1);
  assert(r.stderr.startsWith("Invalid skill catalogue:"), r.stderr);
  assert.equal(r.stdout, "");
}
test("valid catalogue and real source tree preserve complete report output", () => {
  const f = fixture();
  assert.equal(audit(f.root).status, 0);
  assert.equal(audit(resolve(".")).status, 0);
});
test("catalogue schema, names, classifications, booleans and required fields reject invalid data", () => {
  for (const value of [{}, [], null, [null], ["example"]]) reject(value);
  const template = fixture().entry;
  for (const [key, value] of [
    ["name", "../example"],
    ["name", "Example"],
    ["name", "a"],
    ["name", "a".repeat(65)],
    ["name", "two--hyphens"],
    ["name", null],
    ["kind", "unknown"],
    ["kind", []],
    ["invocation", "automatic"],
    ["invocation", {}],
    ["hermes", "false"],
    ["hermes", 1],
    ["references", []],
  ] as const)
    reject([{ ...template, [key]: value }]);
  for (const key of Object.keys(template)) {
    const entry = { ...template };
    delete entry[key];
    reject([entry]);
  }
  reject([{ ...template, unexpected: true }]);
});
test("process entries cannot deploy to Hermes or map references", () => {
  const f = fixture();
  fs.renameSync(join(f.llm, "skills"), join(f.llm, "process-skills"));
  write(
    join(f.llm, "process-skills/example/SKILL.md"),
    "---\ndescription: Process fixture\n---\n",
  );
  f.entry.kind = "process";
  f.entry.references = {};
  write(f.catalog, JSON.stringify([f.entry]));
  assert.equal(audit(f.root).status, 0);
  for (const changes of [
    { hermes: true },
    { references: { "example.md": "guides/example.md" } },
  ]) {
    write(f.catalog, JSON.stringify([{ ...f.entry, ...changes }]));
    assert.equal(audit(f.root).status, 1);
  }
});
test("duplicate skills and nested duplicate JSON keys fail before report generation", () => {
  const f = fixture();
  for (const text of [
    JSON.stringify([f.entry, f.entry]),
    '[{"name":"example","name":"other"}]',
    '[{"references":{"same.md":"a","same.md":"b"}}]',
  ]) {
    write(f.catalog, text);
    const r = audit(f.root);
    assert.equal(r.status, 1);
    assert(r.stderr.includes("duplicate"));
    assert.equal(r.stdout, "");
  }
});
test("mapped source and target paths cannot traverse, escape or select missing/non-file sources", () => {
  const f = fixture();
  for (const source of [
    "../outside.md",
    "/etc/passwd",
    "guides/../guides/example.md",
    "guides//example.md",
    "./guides/example.md",
    "guides\\example.md",
    "guides/missing.md",
    "guides",
    "",
    null,
  ]) {
    write(
      f.catalog,
      JSON.stringify([{ ...f.entry, references: { "example.md": source } }]),
    );
    assert.equal(audit(f.root).status, 1);
  }
  for (const target of [
    "../escape.md",
    "/escape.md",
    "nested/escape.md",
    "bad\\name.md",
    "script.sh",
    ".md",
  ]) {
    write(
      f.catalog,
      JSON.stringify([
        { ...f.entry, references: { [target]: "guides/example.md" } },
      ]),
    );
    assert.equal(audit(f.root).status, 1);
  }
  const outside = join(temporary, "outside.md");
  write(outside, "synthetic private content");
  fs.symlinkSync(outside, join(f.llm, "guides/escape.md"));
  write(
    f.catalog,
    JSON.stringify([
      { ...f.entry, references: { "example.md": "guides/escape.md" } },
    ]),
  );
  const r = audit(f.root);
  assert(r.stderr.includes("escapes source root"));
  assert(!r.stdout.includes("synthetic"));
});
test("local/mapped collisions and missing registered skills fail closed", () => {
  const f = fixture();
  write(join(f.llm, "skills/example/references/example.md"), "local\n");
  assert(audit(f.root).stderr.includes("duplicates local reference"));
  fs.unlinkSync(join(f.llm, "skills/example/references/example.md"));
  fs.unlinkSync(join(f.llm, "skills/example/SKILL.md"));
  assert(audit(f.root).stderr.includes("missing catalogue source"));
});
test("frontmatter descriptions retain folded blocks, quoted values and Unicode character counts", () => {
  const f = fixture();
  for (const description of [
    'description: "Café test"',
    "description: >-\n  Café\n\tsecond line",
    "description: |\n  \n  one line",
    'description: "one"\ndescription: two',
  ]) {
    write(
      join(f.llm, "skills/example/SKILL.md"),
      `---\n${description}\n---\nreferences/example.md\n`,
    );
    assert.equal(audit(f.root).status, 0);
  }
  write(
    join(f.llm, "skills/example/SKILL.md"),
    "description: not frontmatter\n",
  );
  assert.equal(audit(f.root).status, 1);
});
test("unregistered disk skills, missing references and unused local references are findings", () => {
  const f = fixture();
  write(
    join(f.llm, "skills/extra/SKILL.md"),
    "---\ndescription: extra fixture\n---\nreferences/missing.md\n",
  );
  write(join(f.llm, "skills/example/references/unmentioned.md"), "local\n");
  const r = audit(f.root);
  assert.equal(r.status, 1);
  for (const text of [
    "missing invocation classification",
    "absent from sharedSkillNames",
    "not local or mapped",
    "not mentioned by SKILL.md",
  ])
    assert(r.stdout.includes(text));
});
test("manual-only metadata requires both files and Pi flags; process guardrails remain model-invoked", () => {
  const f = fixture();
  f.entry.invocation = "user-invoked router/orchestrator";
  write(f.catalog, JSON.stringify([f.entry]));
  let r = audit(f.root);
  assert.equal(r.status, 1);
  assert(r.stdout.includes("no agents/openai.yaml"));
  assert(r.stdout.includes("missing Pi disable-model-invocation"));
  write(
    join(f.llm, "skills/example/agents/openai.yaml"),
    "policy:\n  allow_implicit_invocation: false\n",
  );
  write(
    join(f.llm, "skills/example/SKILL.md"),
    "---\ndescription: Fixture\ndisable-model-invocation: YES\n---\nreferences/example.md\n",
  );
  assert.equal(audit(f.root).status, 0);
  const guard = {
    name: "superpowers",
    kind: "process",
    invocation: "process guardrail",
    hermes: false,
    references: {},
  };
  write(f.catalog, JSON.stringify([f.entry, guard]));
  write(
    join(f.llm, "process-skills/superpowers/SKILL.md"),
    "---\ndescription: Guardrail\ndisable-model-invocation: true\n---\n",
  );
  assert(audit(f.root).stdout.includes("should stay model-invoked"));
});
test("long-reference contents checks honor the first forty lines and warning exit status", () => {
  const f = fixture(),
    path = join(f.llm, "guides/long.md");
  write(path, "line\n".repeat(101));
  assert.equal(audit(f.root).status, 1);
  write(path, "# Table of Contents\n" + "line\n".repeat(101));
  assert.equal(audit(f.root).status, 0);
  write(path, "line\n".repeat(40) + "## Contents\n" + "line\n".repeat(70));
  assert.equal(audit(f.root).status, 1);
});
test("overlap terms are unique per skill and sorted by count then name", () => {
  const f = fixture(),
    entries = [];
  for (let i = 0; i < 5; i++) {
    const name = "fixture-" + i;
    entries.push({ ...f.entry, name, references: {} });
    write(
      join(f.llm, "skills", name, "SKILL.md"),
      `---\ndescription: Review review verification skills ${i < 4 ? "updates" : ""}\n---\n`,
    );
  }
  fs.rmSync(join(f.llm, "skills/example"), { recursive: true });
  write(f.catalog, JSON.stringify(entries));
  const r = audit(f.root);
  assert.equal(r.status, 0);
  assert(r.stdout.includes("review                   fixture-0"));
  assert(r.stdout.includes("updates                  fixture-0"));
});
test("freshness markers count independently, validate ISO dates and report stale/missing fields", () => {
  const f = fixture(),
    path = join(f.llm, "guides/dates.md");
  for (const date of [
    "2000-01-01",
    "20000101",
    "2020-W53-7",
    "2020W537",
    "2020-W53",
    "2020-W54-1",
    "2023-02-29",
    "not-a-date",
  ]) {
    write(path, `Source: fixture\nChecked: ${date}\nUpdate trigger: change\n`);
    const r = audit(f.root);
    assert.equal(r.status, 1);
    assert(r.stdout.includes("Source=1 Checked=1 Update trigger=1"));
  }
  write(path, "Source: fixture\n");
  assert(audit(f.root).stdout.includes("Source header missing Checked:"));
  write(path, "Checked: 9999-01-01\n");
  assert.equal(audit(f.root).status, 0);
});
test("a symlinked LLM root keeps relative reporting and confined reference resolution", () => {
  const f = fixture(),
    moved = join(temporary, "llm-alias-target");
  fs.renameSync(f.llm, moved);
  fs.symlinkSync(moved, f.llm);
  const r = audit(f.root, false);
  assert.equal(r.status, 0, r.stderr);
  assert(r.stdout.includes("No findings."));
});
test("CLI help and explicit checkout selection never execute repository content", () => {
  assert.equal(spawnSync(bin, ["--help"]).status, 0);
  assert.equal(spawnSync(bin, ["--unknown"]).status, 2);
  const f = fixture(),
    sentinel = join(f.root, "must-not-exist");
  write(join(f.root, "flake.nix"), `builtins.throw "not evaluated"`);
  write(
    join(f.llm, "scripts/audit-skills"),
    `#!/bin/sh\ntouch '${sentinel}'\n`,
  );
  assert.equal(audit(f.root, false).status, 0);
  assert(!fs.existsSync(sentinel));
  assert.equal(
    spawnSync(bin, ["--root", join(temporary, "missing")]).status,
    1,
  );
});
