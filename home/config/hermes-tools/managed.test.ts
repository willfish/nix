import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import test from "node:test";
const bins = process.env.HERMES_MANAGED_BIN_DIR!,
  legacy = process.env.HERMES_MANAGED_LEGACY,
  python = process.env.HERMES_MANAGED_PYTHON || "python3";
assert.ok(bins, "Set HERMES_MANAGED_BIN_DIR");
function put(path: string, data: any) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(
    path,
    typeof data === "string" || Buffer.isBuffer(data)
      ? data
      : JSON.stringify(data),
  );
}
function temp(fn: (root: string) => void) {
  const root = mkdtempSync(join(tmpdir(), "hermes-managed-"));
  try {
    fn(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}
function run(kind: string, args: string[], old = false) {
  const r = spawnSync(
    old ? python : join(bins, "hermes-" + kind),
    old
      ? [
          join(
            legacy!,
            "hermes_" +
              (kind === "declaration" ? "declaration" : "profile") +
              ".py",
          ),
          ...args,
        ]
      : args,
    { encoding: "utf8", timeout: 35000 },
  );
  assert.ifError(r.error);
  assert.doesNotMatch(
    r.stderr,
    /AddressSanitizer|LeakSanitizer|runtime error:|PRIVATE-SENTINEL/,
  );
  return r;
}
function yaml(path: string) {
  const r = spawnSync(process.env.HERMES_YQ || "yq", ["-o=json", ".", path], {
    encoding: "utf8",
  });
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
}
function file(path: string, content: string | Buffer, executable = false) {
  return { path, content: Buffer.from(content).toString("base64"), executable };
}
function declaration(
  files: any[] = [file("config.yaml", "model: fixture\n")],
  jobs: any[] = [],
) {
  return { version: 1, files, seed_files: [], jobs };
}
function apply(root: string, d: any, old = false, extra: string[] = []) {
  const path = join(root, "declaration.json");
  put(path, d);
  return run("declaration", [path, join(root, "home"), ...extra], old);
}
function both(fn: (root: string, old: boolean) => void) {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => fn(root, old));
}
const mode = (path: string) => statSync(path).mode & 0o777;

test("profile overlays preserve unrelated settings and provider secrets, replace matching names and append new providers", () =>
  both((root, old) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json");
    put(
      p,
      "display:\n  theme: custom\n  streaming: false\ncustom_providers:\n  - name: other\n    api_key: retained\n  - name: qwen-local\n    base_url: old\n    options: {keep: true}\n",
    );
    put(o, {
      display: { streaming: true },
      custom_providers: [
        { name: "qwen-local", base_url: "http://127.0.0.1:8081/v1" },
        { name: "new", enabled: true },
      ],
    });
    const r = run("profile", [p, o], old);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Updated Qwen profile/);
    assert.deepEqual(yaml(p), {
      display: { theme: "custom", streaming: true },
      custom_providers: [
        { name: "other", api_key: "retained" },
        {
          name: "qwen-local",
          base_url: "http://127.0.0.1:8081/v1",
          options: { keep: true },
        },
        { name: "new", enabled: true },
      ],
    });
  }));
test("profile backups are private, preserve the prior text, and repeated semantic no-ops preserve inode and bytes", () =>
  both((root, old) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json"),
      before =
        "# keep this in backup\nagent: {max_turns: 90}\ncompression: {enabled: false}\n";
    put(p, before);
    put(o, {
      agent: { max_turns: "unlimited" },
      compression: { enabled: true },
    });
    assert.equal(run("profile", [p, o], old).status, 0);
    const backups = readdirSync(root).filter((x) =>
      x.startsWith("profile.yaml.before-local-llm-"),
    );
    assert.equal(backups.length, 1);
    assert.equal(readFileSync(join(root, backups[0]), "utf8"), before);
    assert.equal(mode(join(root, backups[0])), 0o600);
    assert.equal(mode(p), 0o600);
    const inode = statSync(p).ino,
      data = readFileSync(p);
    assert.equal(run("profile", [p, o], old).stdout, "");
    assert.equal(statSync(p).ino, inode);
    assert.deepEqual(readFileSync(p), data);
    assert.equal(
      readdirSync(root).filter((x) => x.startsWith(".local-llm-")).length,
      0,
    );
  }));
test("YAML scalar types, aliases, merge keys and quoted values survive an unrelated overlay", () =>
  both((root, old) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json");
    put(
      p,
      'base: &base {enabled: yes, retries: 0x10, quoted: "012", empty: null}\ncopy: *base\nmerged:\n  <<: *base\n  retries: 4\ntext: "false"\nfloat: 1.5\n',
    );
    put(o, { other: true });
    assert.equal(run("profile", [p, o], old).status, 0);
    assert.deepEqual(yaml(p), {
      base: { enabled: true, retries: 16, quoted: "012", empty: null },
      copy: { enabled: true, retries: 16, quoted: "012", empty: null },
      merged: { enabled: true, retries: 4, quoted: "012", empty: null },
      text: "false",
      float: 1.5,
      other: true,
    });
  }));
test("YAML 1.1 number forms and quoted numeric credentials retain their scalar meanings", () =>
  temp((root) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json");
    put(
      p,
      'octal: 012\nhex: 0x10\nbase60: 1:20\nfloat60: 1:20.5\nexponent: 1.0e+3\nnotExponent: 1.0e3\nnotOctal: 08\nquoted: "012"\n',
    );
    put(o, { other: true });
    assert.equal(run("profile", [p, o]).status, 0);
    assert.deepEqual(yaml(p), {
      octal: 10,
      hex: 16,
      base60: 80,
      float60: 80.5,
      exponent: 1000,
      notExponent: "1.0e3",
      notOctal: "08",
      quoted: "012",
      other: true,
    });
  }));
test("runtime keys reach only model and qwen-local provider; missing keys do not reset existing secrets", () =>
  both((root, old) => {
    const p = join(root, "deep/profile.yaml"),
      o = join(root, "overlay.json"),
      k = join(root, "key");
    put(o, {
      model: { provider: "qwen-local" },
      custom_providers: [
        { name: "other", api_key: "retained" },
        { name: "qwen-local" },
      ],
    });
    put(k, "\vfixture-runtime-key\u0085\n");
    assert.equal(run("profile", [p, o, "--key-file", k], old).status, 0);
    const v = yaml(p);
    assert.equal(v.model.api_key, "fixture-runtime-key");
    assert.equal(v.custom_providers[1].api_key, "fixture-runtime-key");
    assert.equal(v.custom_providers[0].api_key, "retained");
    assert.ok(!readFileSync(o, "utf8").includes("fixture-runtime-key"));
    put(k, " \n");
    const before = readFileSync(p);
    assert.equal(run("profile", [p, o, "--key-file", k], old).status, 1);
    assert.deepEqual(readFileSync(p), before);
  }));
test("profile symlinks and unsafe YAML tags fail without changing targets or logging input", () => {
  temp((root) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json"),
      target = join(root, "target");
    put(target, "keep");
    symlinkSync(target, p);
    put(o, {});
    assert.equal(run("profile", [p, o]).status, 1);
    assert.equal(readFileSync(target, "utf8"), "keep");
    rmSync(p);
    put(p, "!!python/object/apply:os.system [PRIVATE-SENTINEL]");
    const r = run("profile", [p, o]);
    assert.equal(r.status, 1);
    assert.equal(
      readFileSync(p, "utf8"),
      "!!python/object/apply:os.system [PRIVATE-SENTINEL]",
    );
  });
});
test("fresh declaration is private and idempotent without altering runtime history or OAuth seeds", () =>
  both((root, old) => {
    const home = join(root, "home"),
      d: any = declaration([
        file("config.yaml", "model: fixture\n"),
        file("scripts/run.sh", "#!/bin/sh\necho fixture\n", true),
      ]);
    d.seed_files = [file("auth.json", "initial-oauth")];
    put(join(home, "sessions/history"), "keep");
    assert.equal(apply(root, d, old).status, 0);
    assert.equal(mode(home), 0o700);
    assert.equal(mode(join(home, "config.yaml")), 0o600);
    assert.equal(mode(join(home, "scripts/run.sh")), 0o700);
    assert.equal(
      readFileSync(join(home, ".managed"), "utf8"),
      "home-manager\n",
    );
    assert.equal(
      readFileSync(join(home, "profiles/qwen/.managed"), "utf8"),
      "home-manager\n",
    );
    const inode = statSync(join(home, "config.yaml")).ino;
    put(join(home, "auth.json"), "refreshed-oauth");
    assert.equal(apply(root, d, old).status, 0);
    assert.equal(statSync(join(home, "config.yaml")).ino, inode);
    assert.equal(
      readFileSync(join(home, "auth.json"), "utf8"),
      "refreshed-oauth",
    );
    assert.equal(readFileSync(join(home, "sessions/history"), "utf8"), "keep");
  }));
test("equal schedules retain job history/repeat completion and completed one-shots; changed schedules reset history", () =>
  both((root, old) => {
    const home = join(root, "home"),
      existing = {
        extra: "keep",
        jobs: [
          {
            id: "a",
            schedule: { kind: "cron", expression: "old" },
            last_run_at: "yesterday",
            failure_streak: 3,
            repeat: { completed: 7 },
            state: "completed",
          },
          { id: "gone", schedule: { kind: "once" } },
        ],
      };
    put(join(home, "cron/jobs.json"), existing);
    const d = declaration(undefined, [
      {
        id: "a",
        schedule: { kind: "cron", expression: "old" },
        enabled: true,
        repeat: { times: 9, completed: 0 },
      },
    ]);
    assert.equal(apply(root, d, old).status, 0);
    const jobs = JSON.parse(readFileSync(join(home, "cron/jobs.json"), "utf8"));
    assert.equal(jobs.extra, "keep");
    assert.deepEqual(jobs.jobs, [
      {
        id: "a",
        schedule: { kind: "cron", expression: "old" },
        enabled: false,
        repeat: { times: 9, completed: 7 },
        last_run_at: "yesterday",
        failure_streak: 3,
        state: "completed",
        next_run_at: null,
      },
    ]);
    d.jobs[0].schedule.expression = "new";
    assert.equal(apply(root, d, old).status, 0);
    const fresh = JSON.parse(readFileSync(join(home, "cron/jobs.json"), "utf8"))
      .jobs[0];
    assert.equal(fresh.last_run_at, undefined);
    assert.equal(fresh.repeat.completed, 0);
    assert.equal(fresh.enabled, true);
  }));
test("changed and stale declared files are backed up by content hash before replacement/removal", () =>
  both((root, old) => {
    const home = join(root, "home");
    put(join(home, "SOUL.md"), "old soul");
    const d = declaration([
      file("SOUL.md", "new soul"),
      file("skills/stale.md", "stale"),
    ]);
    assert.equal(apply(root, d, old).status, 0);
    const hash = createHash("sha256").update("old soul").digest("hex");
    assert.equal(
      readFileSync(join(home, "backups/home-manager", hash), "utf8"),
      "old soul",
    );
    assert.equal(mode(join(home, "backups/home-manager", hash)), 0o600);
    d.files.pop();
    assert.equal(apply(root, d, old).status, 0);
    assert.equal(existsSync(join(home, "skills/stale.md")), false);
    assert.equal(
      readFileSync(
        join(
          home,
          "backups/home-manager",
          createHash("sha256").update("stale").digest("hex"),
        ),
        "utf8",
      ),
      "stale",
    );
    assert.deepEqual(
      JSON.parse(readFileSync(join(home, ".home-manager-files.json"), "utf8")),
      ["SOUL.md"],
    );
  }));
test("paths, runtime ownership, invalid payloads and duplicate jobs fail before declared files change", () => {
  for (const bad of [
    "../outside",
    "/tmp/escaped",
    "sessions/data",
    "cron/jobs.json",
    "profiles/other/config.yaml",
  ])
    both((root, old) => {
      const home = join(root, "home");
      put(join(home, "SOUL.md"), "keep");
      const d = declaration([file("SOUL.md", "replace"), file(bad, "bad")]);
      assert.equal(apply(root, d, old).status, 1);
      assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "keep");
    });
  both((root, old) => {
    const home = join(root, "home");
    put(join(home, "SOUL.md"), "keep");
    symlinkSync(root, join(home, "skills"));
    assert.equal(
      apply(
        root,
        declaration([file("SOUL.md", "replace"), file("skills/x", "bad")]),
        old,
      ).status,
      1,
    );
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "keep");
    rmSync(join(home, "skills"));
    const d = declaration(
      [file("SOUL.md", "replace")],
      [{ id: "same" }, { id: "same" }],
    );
    assert.equal(apply(root, d, old).status, 1);
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "keep");
    d.jobs = [];
    d.files[0].content = "not base64!";
    assert.equal(apply(root, d, old).status, 1);
  });
});
test("duplicate normalized file paths keep their original position but use the final payload", () =>
  both((root, old) => {
    const d = declaration([
      file("SOUL.md", "first"),
      file("./SOUL.md", "last"),
    ]);
    assert.equal(apply(root, d, old).status, 0);
    assert.equal(readFileSync(join(root, "home/SOUL.md"), "utf8"), "last");
  }));
test("managed-file index cannot delete runtime files and jobs errors do not partially publish declarations", () =>
  both((root, old) => {
    const home = join(root, "home");
    put(join(home, "SOUL.md"), "keep");
    put(join(home, ".home-manager-files.json"), ["sessions/history"]);
    put(join(home, "sessions/history"), "runtime");
    assert.equal(
      apply(root, declaration([file("SOUL.md", "changed")]), old).status,
      1,
    );
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "keep");
    assert.equal(
      readFileSync(join(home, "sessions/history"), "utf8"),
      "runtime",
    );
    put(join(home, ".home-manager-files.json"), []);
    put(join(home, "cron/jobs.json"), "{invalid");
    assert.equal(
      apply(root, declaration([file("SOUL.md", "changed")]), old).status,
      1,
    );
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "keep");
  }));
test("Qwen overlays reuse runtime keys, seed declared keys, and generate new private keys only when needed", () => {
  for (const which of ["runtime", "declared", "generated"])
    both((root, old) => {
      const home = join(root, "home"),
        overlay = join(root, "overlay.json"),
        key = join(root, "local/api-key");
      const initial =
        which === "declared"
          ? "model: {api_key: declared-key}\n"
          : "model: {}\n";
      const d = declaration([file("profiles/qwen/config.yaml", initial)]);
      put(overlay, {
        model: { default: "new-model" },
        custom_providers: [{ name: "qwen-local" }],
      });
      if (which === "runtime") put(key, " runtime-key\n");
      const args = ["--qwen-overlay", overlay, "--key-file", key];
      assert.equal(apply(root, d, old, args).status, 0);
      const v = yaml(join(home, "profiles/qwen/config.yaml")),
        raw = readFileSync(key, "utf8").trim();
      assert.equal(v.model.api_key, raw);
      assert.equal(v.custom_providers[0].api_key, raw);
      assert.equal(
        raw,
        which === "runtime"
          ? "runtime-key"
          : which === "declared"
            ? "declared-key"
            : raw,
      );
      if (which === "generated") {
        assert.match(raw, /^[0-9a-f]{64}$/);
        assert.equal(mode(key), 0o600);
      }
      const before = readFileSync(join(home, "profiles/qwen/config.yaml"));
      assert.equal(apply(root, d, old, args).status, 0);
      assert.deepEqual(
        readFileSync(join(home, "profiles/qwen/config.yaml")),
        before,
      );
    });
});
test("declaration routes exactly the configured Telegram topic and standalone routing is quiet/idempotent", () =>
  both((root, old) => {
    const home = join(root, "home");
    put(join(home, "house-telegram.json"), {
      group_id: -1001,
      topics: { Qwen: 787 },
    });
    assert.equal(apply(root, declaration(), old).status, 0);
    const config = join(home, "config.yaml"),
      v = yaml(config);
    assert.equal(v.multiplex_profiles, true);
    assert.deepEqual(v.profile_routes, [
      {
        name: "telegram-qwen",
        platform: "telegram",
        chat_id: "-1001",
        thread_id: "787",
        profile: "qwen",
        enabled: true,
      },
    ]);
    assert.deepEqual(v.gateway.profile_routes, v.profile_routes);
    if (!old) {
      const inode = statSync(config).ino,
        r = run("telegram-route", [
          config,
          "--house",
          join(home, "house-telegram.json"),
        ]);
      assert.equal(r.status, 0, r.stderr);
      assert.equal(r.stdout, "");
      assert.equal(statSync(config).ino, inode);
      put(join(home, "house-telegram.json"), { group_id: -1001, topics: {} });
      assert.equal(
        run("telegram-route", [
          config,
          "--house",
          join(home, "house-telegram.json"),
        ]).status,
        0,
      );
      assert.deepEqual(yaml(config).profile_routes, []);
      assert.deepEqual(yaml(config).gateway.profile_routes, []);
    }
  }));
test("invalid gateway and profile shape failures never disclose private config values", () =>
  temp((root) => {
    const p = join(root, "profile.yaml"),
      o = join(root, "overlay.json");
    put(p, "model: PRIVATE-SENTINEL\n");
    put(o, { model: { provider: "qwen" } });
    put(join(root, "key"), "fixture-key");
    assert.equal(
      run("profile", [p, o, "--key-file", join(root, "key")]).status,
      0,
    );
    put(p, "gateway: [PRIVATE-SENTINEL]\n");
    put(join(root, "house.json"), { group_id: "1", topics: { Qwen: 2 } });
    const before = readFileSync(p);
    assert.equal(
      run("telegram-route", [p, "--house", join(root, "house.json")]).status,
      1,
    );
    assert.deepEqual(readFileSync(p), before);
  }));
test("a later filesystem failure retains earlier writes and backups but does not publish the index", () =>
  temp((root) => {
    const home = join(root, "home");
    put(join(home, "SOUL.md"), "old");
    put(join(home, "scripts"), "not a directory");
    const d = declaration([
      file("SOUL.md", "new"),
      file("scripts/file", "content"),
    ]);
    assert.equal(apply(root, d).status, 1);
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "new");
    assert.equal(existsSync(join(home, ".home-manager-files.json")), false);
    assert.equal(existsSync(join(home, ".managed")), false);
    assert.equal(
      readdirSync(home).some((x) => x.startsWith(".hermes-managed-")),
      false,
    );
  }));
test("the declaration waits for the scheduler lock and publishes only after it is released", async () => {
  const root = mkdtempSync(join(tmpdir(), "hermes-lock-"));
  let holder: any, child: any;
  try {
    const home = join(root, "home");
    mkdirSync(join(home, "cron"), { recursive: true });
    const lock = join(home, "cron/.jobs.lock");
    holder = spawn("flock", [lock, "bash", "-c", "printf ready; sleep 1"], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    await new Promise<void>((resolve) =>
      holder.stdout.once("data", () => resolve()),
    );
    const d = join(root, "declaration.json");
    put(d, declaration([file("SOUL.md", "written")]));
    child = spawn(join(bins, "hermes-declaration"), [d, home], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    const completed = new Promise<number | null>((resolve) =>
      child.once("close", resolve),
    );
    await sleep(150);
    assert.equal(existsSync(join(home, "SOUL.md")), false);
    assert.equal(await completed, 0);
    assert.equal(readFileSync(join(home, "SOUL.md"), "utf8"), "written");
  } finally {
    holder?.kill("SIGKILL");
    child?.kill("SIGKILL");
    rmSync(root, { recursive: true, force: true });
  }
});
