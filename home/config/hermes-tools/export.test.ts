import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
const binary = process.env.HERMES_EXPORT_BIN!,
  legacy = process.env.HERMES_EXPORT_LEGACY,
  python = process.env.HERMES_EXPORT_PYTHON || "python3";
assert.ok(binary, "Set HERMES_EXPORT_BIN");
const required = [
  "config.yaml",
  "SOUL.md",
  "AGENTS.md",
  "profiles/qwen/config.yaml",
];
const runtime = [
  "last_run_at",
  "next_run_at",
  "last_status",
  "last_error",
  "last_delivery_error",
  "last_dispatch",
  "failure_streak",
  "fire_claim",
];
function put(path: string, data: string | Buffer, mode = 0o600) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, data, { mode });
  chmodSync(path, mode);
}
function setup() {
  const root = mkdtempSync(join(tmpdir(), "hermes-export-")),
    home = join(root, "home"),
    repo = join(root, "repo"),
    output = join(repo, "secrets/hermes.json"),
    commands = join(root, "bin");
  mkdirSync(commands);
  mkdirSync(dirname(output), { recursive: true });
  for (const file of required) put(join(home, file), "fixture " + file + "\n");
  put(
    join(home, "cron/jobs.json"),
    JSON.stringify({
      jobs: [
        {
          id: "job",
          schedule: { kind: "cron" },
          prompt: "fixture prompt",
          repeat: { times: null, completed: 7 },
          ...Object.fromEntries(runtime.map((k) => [k, "bookkeeping"])),
        },
      ],
    }),
  );
  const script = `#!${process.execPath}\nconst fs=require('node:fs');const input=fs.readFileSync(0);const mode=process.env.SOPS_FIXTURE_MODE;if(mode==='failure'){process.stdout.write(input);process.stderr.write('PRIVATE-FAILURE '+input);process.exit(7);}if(mode==='signal'){process.kill(process.pid,'SIGTERM');}if(mode==='large-error'){process.stderr.write('x'.repeat(200000));process.exit(4);}process.stdout.write(JSON.stringify({declaration:JSON.parse(input),args:process.argv.slice(2),cwd:process.cwd()}));\n`;
  put(join(commands, "sops"), script, 0o700);
  return {
    root,
    home,
    repo,
    output,
    commands,
    env: { PATH: commands + ":" + process.env.PATH, HOME: root },
  };
}
function fixture(fn: (f: ReturnType<typeof setup>) => void) {
  const f = setup();
  try {
    fn(f);
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
}
function run(
  f: ReturnType<typeof setup>,
  old = false,
  args = [f.home, f.output],
  extra: any = {},
) {
  const r = spawnSync(old ? python : binary, old ? [legacy!, ...args] : args, {
    encoding: "utf8",
    timeout: 15000,
    env: { ...f.env, ...extra },
    cwd: f.root,
  });
  assert.ifError(r.error);
  assert.doesNotMatch(
    r.stderr,
    /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:|DEADLYSIGNAL/,
  );
  return r;
}
function captured(f: ReturnType<typeof setup>) {
  return JSON.parse(readFileSync(f.output, "utf8"));
}
function parity(f: ReturnType<typeof setup>) {
  const r = run(f);
  assert.equal(r.status, 0, r.stderr);
  const result = captured(f);
  if (legacy) {
    const old = run(f, true);
    assert.equal(old.status, 0, old.stderr);
    assert.deepEqual(captured(f), result);
    assert.equal(r.stdout, old.stdout);
  }
  return result;
}
function bytes(item: any) {
  return Buffer.from(item.content, "base64");
}

test("captures required files and binary assets, flags executables, strips scheduler history and seeds OAuth separately", () =>
  fixture((f) => {
    put(join(f.home, "scripts/run.sh"), "#!/bin/sh\nprintf fixture\n", 0o710);
    put(join(f.home, "assets/bytes"), Buffer.from([0, 255, 65, 10]));
    put(join(f.home, "auth.json"), "fixture-auth");
    put(join(f.home, "profiles/qwen/auth.json"), "fixture-qwen-auth");
    put(join(f.home, "sessions/private.json"), "must not export");
    const result = parity(f),
      d = result.declaration;
    assert.equal(d.version, 1);
    assert.deepEqual(
      d.files.map((x: any) => x.path),
      [...required, "scripts/run.sh", "assets/bytes"],
    );
    assert.equal(
      d.files.find((x: any) => x.path === "scripts/run.sh").executable,
      true,
    );
    assert.deepEqual(bytes(d.files.at(-1)), Buffer.from([0, 255, 65, 10]));
    assert.deepEqual(
      d.seed_files.map((x: any) => x.path),
      ["auth.json", "profiles/qwen/auth.json"],
    );
    assert.deepEqual(d.jobs, [
      {
        id: "job",
        schedule: { kind: "cron" },
        prompt: "fixture prompt",
        repeat: { times: null, completed: 0 },
      },
    ]);
    assert.equal(result.cwd, f.repo);
    assert.deepEqual(result.args, [
      "--encrypt",
      "--input-type",
      "json",
      "--output-type",
      "json",
      "--filename-override",
      f.output,
      "/dev/stdin",
    ]);
  }));
test("recursive capture is component-sorted and skips hidden/cache/managed-script ownership", () =>
  fixture((f) => {
    for (const file of [
      "scripts/a/z",
      "scripts/a.md",
      "scripts/b",
      "skills/example/SKILL.md",
      "skills/.private.md",
      "skills/example/.state",
      "skills/node_modules/vendor/a",
      "skills/__pycache__/cache",
      "scripts/nested/apply-house-telegram.py",
      "scripts/weekly-hermes-update.sh",
    ])
      put(join(f.home, file), "fixture");
    const d = parity(f).declaration;
    assert.deepEqual(
      d.files.slice(4).map((x: any) => x.path),
      ["skills/example/SKILL.md", "scripts/a/z", "scripts/a.md", "scripts/b"],
    );
  }));
test("mem0 plugin relocation preserves its broader discovery rules and first-owner deduplication", () =>
  fixture((f) => {
    put(join(f.home, "plugins/mem0-selfhosted/config.txt"), "declared-first");
    for (const [name, value] of [
      ["config.txt", "second"],
      [".hidden", "plugin-hidden"],
      ["node_modules/fixture", "plugin-node"],
      ["__pycache__/cache", "ignored"],
      ["memory.py", "plugin-source"],
    ])
      put(
        join(f.home, "hermes-agent/plugins/memory/mem0-selfhosted", name),
        value,
      );
    const files = parity(f).declaration.files;
    const plugin = files.filter((x: any) =>
      x.path.startsWith("plugins/mem0-selfhosted/"),
    );
    assert.deepEqual(
      plugin.map((x: any) => x.path),
      [
        "plugins/mem0-selfhosted/config.txt",
        "plugins/mem0-selfhosted/.hidden",
        "plugins/mem0-selfhosted/memory.py",
        "plugins/mem0-selfhosted/node_modules/fixture",
      ],
    );
    assert.equal(bytes(plugin[0]).toString(), "declared-first");
  }));
test("leaf symlinks stay owned elsewhere; required symlinks must still resolve to regular files", () =>
  fixture((f) => {
    put(join(f.root, "linked"), "linked data");
    rmSync(join(f.home, "SOUL.md"));
    symlinkSync(join(f.root, "linked"), join(f.home, "SOUL.md"));
    symlinkSync(join(f.root, "linked"), join(f.home, "auth.json"));
    mkdirSync(join(f.home, "assets"));
    symlinkSync(join(f.root, "linked"), join(f.home, "assets/link"));
    mkdirSync(join(f.root, "directory"));
    put(join(f.root, "directory/skip"), "not followed");
    symlinkSync(join(f.root, "directory"), join(f.home, "assets/directory"));
    const d = parity(f).declaration;
    assert.deepEqual(
      d.files.map((x: any) => x.path),
      required.filter((x) => x !== "SOUL.md"),
    );
    assert.deepEqual(d.seed_files, []);
    rmSync(join(f.root, "linked"));
    put(f.output, "previous encrypted output");
    for (const old of legacy ? [false, true] : [false]) {
      assert.equal(run(f, old).status, 1);
      assert.equal(readFileSync(f.output, "utf8"), "previous encrypted output");
    }
  }));
test("store references are allowed only in actual Markdown suffixes and never in captured non-Markdown", () => {
  fixture((f) => {
    put(join(f.home, "scripts/reference.md"), "/nix/store/fixture-path");
    assert.ok(
      parity(f).declaration.files.some(
        (x: any) => x.path === "scripts/reference.md",
      ),
    );
  });
  for (const name of ["run.sh", "README.MD", ".md", "binary"])
    fixture((f) => {
      put(
        join(
          f.home,
          name === ".md"
            ? "hermes-agent/plugins/memory/mem0-selfhosted"
            : "scripts",
          name,
        ),
        Buffer.concat([
          Buffer.from([0]),
          Buffer.from("/nix/store/fixture-sensitive"),
        ]),
      );
      put(f.output, "old ciphertext");
      for (const old of legacy ? [false, true] : [false]) {
        const r = run(f, old);
        assert.equal(r.status, 1);
        assert.equal(readFileSync(f.output, "utf8"), "old ciphertext");
        if (!old)
          assert.doesNotMatch(r.stderr, /fixture-sensitive|\/nix\/store/);
      }
    });
});
test("all runtime fields are removed only at the top level, while other jobs and repeat fields remain", () =>
  fixture((f) => {
    const jobs = [
      {
        id: "one",
        enabled: false,
        state: "completed",
        repeat: { times: 5, completed: 9, other: true },
        nested: { last_run_at: "keep" },
        ...Object.fromEntries(runtime.map((k) => [k, "drop"])),
      },
      { id: "two", repeat: null },
      { id: "three", repeat: 5 },
      { id: "four", repeat: {} },
    ];
    put(
      join(f.home, "cron/jobs.json"),
      JSON.stringify({ jobs, extra: "not exported" }),
    );
    const result = parity(f).declaration.jobs;
    assert.deepEqual(result, [
      {
        id: "one",
        enabled: false,
        state: "completed",
        repeat: { times: 5, completed: 0, other: true },
        nested: { last_run_at: "keep" },
      },
      { id: "two", repeat: null },
      { id: "three", repeat: 5 },
      { id: "four", repeat: { completed: 0 } },
    ]);
  }));
test("missing required config and malformed job data stop before SOPS or destination mutation", () => {
  for (const type of [
    "missing",
    "directory",
    "bad-json",
    "missing-jobs",
    "nonobject-job",
  ])
    fixture((f) => {
      if (type === "missing") rmSync(join(f.home, "AGENTS.md"));
      if (type === "directory") {
        rmSync(join(f.home, "AGENTS.md"));
        mkdirSync(join(f.home, "AGENTS.md"));
      }
      if (type === "bad-json") put(join(f.home, "cron/jobs.json"), "bad-json");
      if (type === "missing-jobs") put(join(f.home, "cron/jobs.json"), "{}");
      if (type === "nonobject-job")
        put(join(f.home, "cron/jobs.json"), '{"jobs":[4]}');
      put(f.output, "old ciphertext");
      for (const old of legacy ? [false, true] : [false]) {
        assert.equal(run(f, old).status, 1);
        assert.equal(readFileSync(f.output, "utf8"), "old ciphertext");
      }
    });
});
test("failed encryption suppresses stdout/stderr even when SOPS echoes the complete plaintext", () =>
  fixture((f) => {
    put(join(f.home, "auth.json"), "DO-NOT-PRINT-CREDENTIAL");
    put(f.output, "old ciphertext");
    for (const mode of ["failure", "signal", "large-error"])
      for (const old of legacy ? [false, true] : [false]) {
        const r = run(f, old, undefined, { SOPS_FIXTURE_MODE: mode });
        assert.equal(r.status, 1);
        assert.equal(r.stdout, "");
        assert.match(
          r.stderr,
          /SOPS encryption failed; no declaration written/,
        );
        assert.doesNotMatch(r.stderr, /PRIVATE-FAILURE|DO-NOT-PRINT|content/);
        assert.equal(readFileSync(f.output, "utf8"), "old ciphertext");
      }
  }));
test("relative outputs retain filename override and policy cwd; output links and modes are preserved", () =>
  fixture((f) => {
    const args = [f.home, "repo/secrets/hermes.json"];
    let r = run(f, false, args);
    assert.equal(r.status, 0, r.stderr);
    assert.equal(captured(f).args.at(-2), "repo/secrets/hermes.json");
    assert.equal(captured(f).cwd, f.repo);
    const target = join(f.root, "other/secrets/export.json");
    put(target, "old", 0o640);
    rmSync(f.output);
    symlinkSync(target, f.output);
    r = run(f);
    assert.equal(r.status, 0, r.stderr);
    assert.equal(realpathSync(f.output), target);
    assert.equal(statSync(target).mode & 0o777, 0o640);
    assert.equal(captured(f).cwd, join(f.root, "other"));
    if (legacy) {
      const result = captured(f);
      r = run(f, true);
      assert.equal(r.status, 0, r.stderr);
      assert.deepEqual(captured(f), result);
    }
  }));
test("CLI misuse and unavailable encryption commands fail without exposing source data", () =>
  fixture((f) => {
    for (const args of [[], [f.home], [f.home, f.output, "extra"], ["--bad"]])
      assert.equal(run(f, false, args).status, 2);
    for (const args of [["-h"], ["--help"], ["--h"]])
      assert.equal(run(f, false, args).status, 0);
    rmSync(join(f.commands, "sops"));
    const r = run(f, false, undefined, { PATH: f.commands });
    assert.equal(r.status, 1);
    assert.equal(r.stdout, "");
    assert.equal(existsSync(f.output), false);
  }));
test("real SOPS encryption and age decryption round-trip disposable credentials without a plaintext output", () =>
  fixture((f) => {
    const age = spawnSync("age-keygen", ["-o", join(f.root, "age.key")], {
      encoding: "utf8",
    });
    assert.equal(age.status, 0, "ephemeral age key generation failed");
    const pub = spawnSync("age-keygen", ["-y", join(f.root, "age.key")], {
      encoding: "utf8",
    });
    assert.equal(pub.status, 0);
    put(
      join(f.repo, ".sops.yaml"),
      "creation_rules:\n  - path_regex: .*\n    age: " +
        pub.stdout.trim() +
        "\n",
    );
    put(join(f.home, "auth.json"), "fixture-private-auth-token");
    const env = {
      PATH: process.env.PATH!,
      HOME: f.root,
      SOPS_AGE_KEY_FILE: join(f.root, "age.key"),
    };
    const r = spawnSync(binary, [f.home, f.output], {
      env,
      encoding: "utf8",
      timeout: 15000,
    });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stderr, "");
    const encrypted = readFileSync(f.output, "utf8");
    assert.match(encrypted, /ENC\[AES256_GCM/);
    assert.doesNotMatch(
      encrypted,
      /fixture-private-auth-token|Zml4dHVyZS1wcml2YXRlLWF1dGgtdG9rZW4=/,
    );
    const decrypted = spawnSync("sops", ["--decrypt", f.output], {
      env,
      encoding: "utf8",
      timeout: 15000,
    });
    assert.equal(decrypted.status, 0, "fixture decryption failed");
    const data = JSON.parse(decrypted.stdout);
    assert.equal(data.version, 1);
    assert.equal(
      bytes(data.seed_files[0]).toString(),
      "fixture-private-auth-token",
    );
    assert.equal(statSync(join(f.root, "age.key")).mode & 0o077, 0);
  }));
