import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import {
  chmodSync,
  existsSync,
  lstatSync,
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
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import test from "node:test";
const bins = process.env.PI_CONFIG_BIN_DIR!,
  failures = process.env.PI_CONFIG_FAILURES!,
  legacy = process.env.PI_CONFIG_LEGACY,
  python = process.env.PI_CONFIG_PYTHON || "python3";
assert.ok(bins && failures, "set native binary directory and failure library");
const OM = "observational-memory-jev",
  marker = ".om-default-off-migrated";
function temp(fn: (root: string) => void) {
  const path = mkdtempSync(join(tmpdir(), "pi-config-"));
  try {
    fn(path);
  } finally {
    rmSync(path, { recursive: true, force: true });
  }
}
function write(path: string, value: any) {
  writeFileSync(
    path,
    typeof value === "string" ? value : JSON.stringify(value),
  );
}
function command(kind: string, args: string[], old = false, options: any = {}) {
  const r = spawnSync(
    old ? python : join(bins, "pi-merge-" + kind),
    old ? [legacy!.replace("{kind}", kind), ...args] : args,
    { encoding: "utf8", timeout: 15000, ...options },
  );
  assert.ifError(r.error);
  assert.doesNotMatch(
    r.stderr,
    /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:|DEADLYSIGNAL/,
  );
  return r;
}
function json(path: string) {
  return JSON.parse(readFileSync(path, "utf8"));
}
function settings(defaults: any, value: any, expected: any, status = 0) {
  let result: Buffer | undefined;
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const file = join(root, "settings.json"),
        declared = join(root, "defaults.json");
      write(declared, defaults);
      if (value !== undefined) write(file, value);
      const r = command("settings", [declared, file], old);
      assert.equal(r.status, status, r.stderr);
      if (!status && existsSync(file)) {
        if (expected !== undefined) assert.deepEqual(json(file), expected);
        if (!old) result = readFileSync(file);
        else assert.deepEqual(readFileSync(file), result);
      }
    });
}
function auth(root: string, old = false, extra: string[] = []) {
  return command("auth", [join(root, "auth.json"), ...extra], old);
}
function seed(root: string, provider = "openai-codex") {
  writeFileSync(join(root, "refresh"), "  fixture-refresh\r\n");
  writeFileSync(join(root, "account"), "\u2003fixture-account\t");
  return [
    "--oauth-provider",
    provider,
    "--refresh-file",
    join(root, "refresh"),
    "--account-file",
    join(root, "account"),
  ];
}
function fault(path: string, mode: string, extra: any = {}) {
  return {
    ...process.env,
    LD_PRELOAD: [process.env.PI_CONFIG_ASAN_RT, failures]
      .filter(Boolean)
      .join(":"),
    PI_CONFIG_FAILURE_PATH: path,
    PI_CONFIG_FAIL: mode,
    ...extra,
  };
}

test("settings fill only missing top-level keys, including false/null and unknown nested objects", () => {
  settings(
    { a: 1, nested: { a: 1, b: 2 }, nil: 3, flag: true },
    { nested: { a: 9 }, nil: null, flag: false, keep: [1, 2] },
    { nested: { a: 9 }, nil: null, flag: false, keep: [1, 2], a: 1 },
  );
  settings({ a: 1 }, undefined, { a: 1 });
  settings({ a: 1 }, "", { a: 1 });
});
test("all five old provider/model pairs retarget to current declarations; unrelated pairs are retained", () => {
  for (const [provider, model] of [
    ["xai", "grok-4.6"],
    ["xai", "grok-4.7"],
    ["opencode-go", "deepseek-v4.1-flash"],
    ["opencode-go", "glm-5.3"],
    ["opencode-go", "space-bunny-free"],
  ])
    settings(
      { defaultProvider: "declared", defaultModel: "model" },
      { defaultProvider: provider, defaultModel: model, extra: 1 },
      { defaultProvider: "declared", defaultModel: "model", extra: 1 },
    );
  settings(
    { defaultProvider: "declared", defaultModel: "model" },
    { defaultProvider: "xai", defaultModel: "grok-next" },
    { defaultProvider: "xai", defaultModel: "grok-next" },
  );
  settings(
    {},
    { defaultProvider: "xai", defaultModel: "grok-4.7" },
    { defaultProvider: null, defaultModel: null },
  );
  settings(
    { defaultModel: "grok-4.7" },
    { defaultProvider: "xai" },
    { defaultProvider: null, defaultModel: "grok-4.7" },
  );
});
test("unhashable provider/model settings fail before writing rather than bypassing retarget checks", () => {
  for (const key of ["defaultProvider", "defaultModel"])
    for (const v of [[], {}])
      settings({ new: true }, { [key]: v }, undefined, 1);
});
test("no-op settings/auth retain original bytes, inode, mode and absent directories", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const kind of ["auth", "settings"])
      temp((root) => {
        const file = join(root, kind + ".json");
        writeFileSync(file, '{ "keep": 1 }');
        chmodSync(file, 0o640);
        const ino = statSync(file).ino;
        write(join(root, "defaults.json"), {});
        const r =
          kind === "auth"
            ? auth(root, old)
            : command("settings", [join(root, "defaults.json"), file], old);
        assert.equal(r.status, 0, r.stderr);
        assert.equal(readFileSync(file, "utf8"), '{ "keep": 1 }');
        assert.equal(statSync(file).ino, ino);
        assert.equal(statSync(file).mode & 0o777, 0o640);
        const absent = join(root, "missing/file");
        assert.equal(
          command(
            kind,
            kind === "auth" ? [absent] : [join(root, "defaults.json"), absent],
            old,
          ).status,
          0,
        );
        assert.equal(existsSync(join(root, "missing")), false);
      });
});
test("one-time memory migration changes exactly boolean true, then records a private marker", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const file = join(root, "settings.json"),
        defaults = join(root, "defaults.json");
      write(defaults, { [OM]: { enabledByDefault: false } });
      write(file, { [OM]: { enabledByDefault: true, other: "keep" } });
      assert.equal(command("settings", [defaults, file], old).status, 0);
      assert.deepEqual(json(file), {
        [OM]: { enabledByDefault: false, other: "keep" },
      });
      assert.equal(
        readFileSync(join(root, marker), "utf8"),
        "observational memory home default is off\n",
      );
      assert.equal(statSync(join(root, marker)).mode & 0o777, 0o600);
      write(file, { [OM]: { enabledByDefault: true } });
      assert.equal(command("settings", [defaults, file], old).status, 0);
      assert.equal(json(file)[OM].enabledByDefault, true);
    });
  for (const v of [false, null, 0, 1, "true", [], {}])
    settings(
      { [OM]: { enabledByDefault: false } },
      { [OM]: { enabledByDefault: v } },
      { [OM]: { enabledByDefault: v } },
    );
});
test("marker-only runs do not rewrite settings, and undeclared/nonboolean defaults never mark", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const file = join(root, "settings.json"),
        defaults = join(root, "defaults.json");
      write(file, { [OM]: null });
      const ino = statSync(file).ino;
      write(defaults, { [OM]: { enabledByDefault: false } });
      assert.equal(command("settings", [defaults, file], old).status, 0);
      assert.equal(statSync(file).ino, ino);
      assert.ok(existsSync(join(root, marker)));
    });
  for (const value of [undefined, null, true, 0, 1, "false"])
    for (const old of legacy ? [false, true] : [false])
      temp((root) => {
        const file = join(root, "settings.json"),
          defaults = join(root, "defaults.json");
        write(file, {});
        write(
          defaults,
          value === undefined ? {} : { [OM]: { enabledByDefault: value } },
        );
        assert.equal(command("settings", [defaults, file], old).status, 0);
        assert.equal(existsSync(join(root, marker)), false);
      });
});
test("existing marker directories and valid symlinks suppress migration; dangling marker links are followed when written", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const mode of ["directory", "symlink", "dangling"])
      temp((root) => {
        const file = join(root, "settings.json"),
          defaults = join(root, "defaults.json"),
          mark = join(root, marker);
        write(defaults, { [OM]: { enabledByDefault: false } });
        write(file, { [OM]: { enabledByDefault: true } });
        if (mode === "directory") mkdirSync(mark);
        else {
          if (mode === "symlink") writeFileSync(join(root, "target"), "keep");
          symlinkSync("target", mark);
        }
        assert.equal(command("settings", [defaults, file], old).status, 0);
        assert.equal(json(file)[OM].enabledByDefault, mode !== "dangling");
        if (mode === "dangling")
          assert.equal(
            readFileSync(join(root, "target"), "utf8"),
            "observational memory home default is off\n",
          );
      });
});
test("auth removes named providers only and seeds missing/non-OAuth entries with the exact schema", () => {
  for (const current of [
    undefined,
    null,
    42,
    "value",
    [],
    {},
    { type: "api_key", key: "obsolete" },
    { type: "OAuth" },
  ]) {
    let native: Buffer | undefined;
    for (const old of legacy ? [false, true] : [false])
      temp((root) => {
        const opts = seed(root);
        write(join(root, "auth.json"), {
          other: { type: "api_key", key: "keep" },
          openai: { key: "drop" },
          ...(current === undefined ? {} : { "openai-codex": current }),
        });
        const r = auth(root, old, ["--drop", "openai", ...opts]);
        assert.equal(r.status, 0, r.stderr);
        const expected = {
          other: { type: "api_key", key: "keep" },
          "openai-codex": {
            type: "oauth",
            refresh: "fixture-refresh",
            accountId: "fixture-account",
            access: "",
            expires: 0,
          },
        };
        assert.deepEqual(json(join(root, "auth.json")), expected);
        assert.equal(statSync(join(root, "auth.json")).mode & 0o777, 0o600);
        if (old)
          assert.deepEqual(readFileSync(join(root, "auth.json")), native);
        else native = readFileSync(join(root, "auth.json"));
      });
  }
});
test("existing OAuth state is retained even if incomplete or expired; dropping the same provider seeds afresh", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const opts = seed(root);
      write(join(root, "auth.json"), { "openai-codex": { type: "oauth" } });
      assert.equal(auth(root, old, opts).status, 0);
      assert.deepEqual(json(join(root, "auth.json")), {
        "openai-codex": { type: "oauth" },
      });
      assert.equal(
        auth(root, old, ["--drop", "openai-codex", ...opts]).status,
        0,
      );
      assert.equal(
        json(join(root, "auth.json"))["openai-codex"].refresh,
        "fixture-refresh",
      );
    });
});
test("missing or empty secret pairs disable seeding but do not suppress provider removals", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const mode of [
      "missing-refresh",
      "missing-account",
      "empty-refresh",
      "empty-account",
    ])
      temp((root) => {
        const opts = seed(root);
        write(join(root, "auth.json"), { old: 1, keep: 2 });
        const file = join(
          root,
          mode.endsWith("refresh") ? "refresh" : "account",
        );
        if (mode.startsWith("missing")) rmSync(file);
        else writeFileSync(file, " \u2003\n\x1c");
        assert.equal(auth(root, old, ["--drop", "old", ...opts]).status, 0);
        assert.deepEqual(json(join(root, "auth.json")), { keep: 2 });
      });
});
test("secret values retain interior whitespace/NUL and escaped JSON while Unicode ends are stripped", () => {
  let native: Buffer | undefined;
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const opts = seed(root);
      writeFileSync(
        join(root, "refresh"),
        "\x1c\u2003尾\r\nline\rkeep\0value\u00a0",
      );
      writeFileSync(join(root, "account"), '\taccount"\\\0end\u3000');
      assert.equal(auth(root, old, opts).status, 0);
      assert.equal(
        json(join(root, "auth.json"))["openai-codex"].refresh,
        "尾\nline\nkeep\0value",
      );
      if (old) assert.deepEqual(readFileSync(join(root, "auth.json")), native);
      else native = readFileSync(join(root, "auth.json"));
    });
});
test("both secret files are validated before even an unchanged auth entry is loaded", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const opts = seed(root);
      const path = join(root, "auth.json");
      write(path, { "openai-codex": { type: "oauth" } });
      const before = readFileSync(path);
      writeFileSync(join(root, "refresh"), Buffer.from([255]));
      assert.equal(auth(root, old, opts).status, 1);
      assert.deepEqual(readFileSync(path), before);
      rmSync(join(root, "account"));
      assert.equal(auth(root, old, opts).status, 0);
    });
});
test("malformed, nonobject, raw NUL and invalid UTF-8 JSON fail without replacing files or creating markers", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const value of [
      " ",
      "[1]",
      "null",
      "true",
      "1",
      '{"x":1,}',
      '{"x":"\\q"}',
      "{}\0",
      "\ufeff{}",
      Buffer.from([255]),
    ])
      for (const kind of ["auth", "settings"])
        temp((root) => {
          const file = join(root, kind + ".json");
          writeFileSync(file, value);
          write(join(root, "defaults.json"), {
            new: 1,
            [OM]: { enabledByDefault: false },
          });
          const before = readFileSync(file);
          const r = command(
            kind,
            kind === "auth"
              ? [file, "--drop", "x"]
              : [join(root, "defaults.json"), file],
            old,
          );
          assert.equal(r.status, 1, r.stderr);
          assert.deepEqual(readFileSync(file), before);
          assert.equal(existsSync(join(root, marker)), false);
          assert.equal(existsSync(file + ".tmp"), false);
        });
  for (const v of ["", [], null, 1]) settings(v, {}, undefined, 1);
});
test("JSON round trips preserve duplicate-key order, Unicode surrogates, NUL keys, arbitrary integers and constants", () => {
  const value =
    '{"keep":1,"dup":0,"dup":2,"nul\\u0000":1,"surrogate":"\\ud800","pair":"\\ud83d\\ude00","numbers":[-0,-0.0,18446744073709551615,1e-5,1e16,NaN,Infinity,-Infinity,1e999]}';
  settings({ new: 1 }, value, undefined);
  settings({ new: 1 }, '{"big":' + "1".repeat(4300) + "}", undefined);
  settings({ new: 1 }, '{"big":' + "1".repeat(4301) + "}", undefined, 1);
});
test("deterministic real values retain the legacy shortest-decimal pretty output", () => {
  let seed = 73;
  const data = Buffer.alloc(8),
    values = [];
  for (let i = 0; i < 500; i++) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    data.writeUInt32LE(seed, 0);
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    data.writeUInt32LE(seed, 4);
    const n = data.readDoubleLE();
    if (Number.isFinite(n)) values.push(n.toExponential(17));
  }
  settings({ new: 1 }, '{"values":[' + values.join(",") + "]}", undefined);
});
test("input is not constrained to panel payload size and deep no-op objects remain unchanged", () => {
  settings(
    { new: true },
    { text: "🙂".repeat(10000) },
    { text: "🙂".repeat(10000), new: true },
  );
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const path = join(root, "auth.json"),
        value = '{"nested":' + "[".repeat(5000) + "0" + "]".repeat(5000) + "}";
      writeFileSync(path, value);
      assert.equal(auth(root, old).status, 0);
      assert.equal(readFileSync(path, "utf8"), value);
    });
});
test("pretty dumps support nesting beyond the streaming panel encoder and excessive decoding fails cleanly", () => {
  for (const leaf of ["0", "0.5", "[]", "{}", '"x"'])
    settings(
      { new: true },
      '{"deep":' + "[".repeat(1200) + leaf + "]".repeat(1200) + "}",
      undefined,
    );
  settings(
    { new: true },
    '{"deep":' + "[".repeat(10000) + "0" + "]".repeat(10000) + "}",
    undefined,
    1,
  );
});
test("negative-looking provider arguments retain argparse handling", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const path = join(root, "auth.json");
      write(path, { "-1": 1, "-.5": 2, "-١": 3, keep: 4 });
      assert.equal(
        command(
          "auth",
          [path, "--drop", "-1", "--drop", "-.5", "--drop", "-١"],
          old,
        ).status,
        0,
      );
      assert.deepEqual(json(path), { keep: 4 });
    });
});
test("parent and destination links, raw relative spelling and dot-dot traversal preserve Pathlib behavior", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const defaults = join(root, "defaults.json");
      write(defaults, { new: 1 });
      mkdirSync(join(root, "actual"));
      symlinkSync("actual", join(root, "alias"));
      write(join(root, "target"), { keep: 2 });
      symlinkSync("../target", join(root, "actual/settings.json"));
      assert.equal(
        command("settings", [defaults, root + "/alias/./settings.json//"], old)
          .status,
        0,
      );
      assert.equal(
        lstatSync(join(root, "actual/settings.json")).isSymbolicLink(),
        false,
      );
      assert.deepEqual(json(join(root, "actual/settings.json")), {
        keep: 2,
        new: 1,
      });
      assert.deepEqual(json(join(root, "target")), { keep: 2 });
      assert.equal(
        command("settings", [defaults, root + "/missing/../other.json"], old)
          .status,
        0,
      );
      assert.ok(existsSync(join(root, "missing")));
    });
});
test("CLI validation happens before mutations, drop repeats, option abbreviations and equals forms work", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const file = join(root, "auth.json");
      for (const args of [
        [],
        [file, "extra"],
        [file, "--unknown"],
        [file, "--drop"],
        [file, "--oauth-provider", "p"],
        [file, "--refresh-file", "a", "--oauth-provider", "p"],
      ])
        assert.equal(command("auth", args, old).status, 2);
      assert.equal(existsSync(file), false);
      write(file, { x: 1, y: 2, z: 3 });
      assert.equal(
        command("auth", [file, "--dr=x", "--drop", "y", "--drop", "x"], old)
          .status,
        0,
      );
      assert.deepEqual(json(file), { z: 3 });
      assert.equal(command("auth", [file, "--oauth-provider="], old).status, 0);
      for (const args of [
        ["-h"],
        ["--unknown", "-h"],
        ["one", "two", "--help"],
        ["-hh"],
      ])
        assert.equal(command("auth", args, old).status, 0);
      for (const args of [
        ["--drop", "-h"],
        ["--help=x", "-h"],
        ["-h=x", "--help"],
      ])
        assert.equal(command("auth", args, old).status, 2);
      assert.equal(command("settings", [], old).status, 2);
    });
});
test(
  "unsearchable parent errors are not mistaken for absent configuration or secrets",
  { skip: process.getuid?.() === 0 },
  () => {
    for (const old of legacy ? [false, true] : [false])
      temp((root) => {
        const locked = join(root, "locked");
        mkdirSync(locked);
        write(join(locked, "auth.json"), { keep: 1 });
        writeFileSync(join(root, "account"), "account");
        chmodSync(locked, 0);
        try {
          assert.equal(
            command("auth", [join(locked, "auth.json")], old).status,
            1,
          );
          assert.equal(
            command(
              "auth",
              [
                join(root, "auth.json"),
                "--refresh-file",
                join(locked, "secret"),
                "--account-file",
                join(root, "account"),
              ],
              old,
            ).status,
            1,
          );
        } finally {
          chmodSync(locked, 0o700);
        }
      });
  },
);
test("private staging and short writes preserve the final output", () => {
  for (const kind of ["auth", "settings"])
    temp((root) => {
      const path = join(root, kind + ".json"),
        defaults = join(root, "defaults.json");
      write(path, { drop: 1, keep: "x".repeat(10000) });
      write(defaults, { new: true });
      const r = command(
        kind,
        kind === "auth" ? [path, "--drop", "drop"] : [defaults, path],
        false,
        { env: fault(path + ".tmp", "short-write") },
      );
      assert.equal(r.status, 0, r.stderr);
      assert.equal(statSync(path).mode & 0o777, 0o600);
      assert.equal(existsSync(path + ".tmp"), false);
      assert.equal(json(path).keep.length, 10000);
    });
});
test("write/chmod/close/rename failures preserve the original settings and omit the memory marker", () => {
  for (const mode of ["create", "write", "chmod", "close", "rename"])
    temp((root) => {
      const defaults = join(root, "defaults.json"),
        path = join(root, "settings.json");
      write(defaults, { [OM]: { enabledByDefault: false } });
      write(path, { [OM]: { enabledByDefault: true } });
      const before = readFileSync(path);
      assert.equal(
        command("settings", [defaults, path], false, {
          env: fault(path + ".tmp", mode),
        }).status,
        1,
      );
      assert.deepEqual(readFileSync(path), before);
      assert.equal(existsSync(join(root, marker)), false);
      if (mode !== "create") {
        assert.equal(statSync(path + ".tmp").mode & 0o777, 0o600);
      }
      assert.equal(command("settings", [defaults, path]).status, 0);
      assert.equal(json(path)[OM].enabledByDefault, false);
      assert.ok(existsSync(join(root, marker)));
    });
});
test("marker failure after settings publication does not roll back the successful merge", () => {
  temp((root) => {
    const defaults = join(root, "defaults.json"),
      path = join(root, "settings.json");
    write(defaults, { [OM]: { enabledByDefault: false } });
    write(path, { [OM]: { enabledByDefault: true } });
    assert.equal(
      command("settings", [defaults, path], false, {
        env: fault(join(root, marker), "create"),
      }).status,
      1,
    );
    assert.equal(json(path)[OM].enabledByDefault, false);
    assert.equal(existsSync(join(root, marker)), false);
    assert.equal(command("settings", [defaults, path]).status, 0);
    assert.ok(existsSync(join(root, marker)));
  });
});
test("staged auth is mode 0600 before credentials are written, including preexisting permissive temporaries", async () => {
  const root = mkdtempSync(join(tmpdir(), "pi-config-publication-"));
  try {
    const path = join(root, "auth.json"),
      release = join(root, "release");
    write(path, { keep: 1 });
    writeFileSync(path + ".tmp", "old");
    chmodSync(path + ".tmp", 0o666);
    const args = seed(root);
    const child = spawn(join(bins, "pi-merge-auth"), [path, ...args], {
      env: fault(path + ".tmp", "", { PI_CONFIG_RELEASE: release }),
      stdio: ["ignore", "ignore", "pipe"],
    });
    let stderr = "";
    child.stderr.on("data", (b) => (stderr += b));
    const ended = new Promise<number | null>((resolve) =>
      child.on("close", resolve),
    );
    for (
      let i = 0;
      i < 500 && (statSync(path + ".tmp").mode & 0o777) !== 0o600;
      i++
    )
      await sleep(5);
    assert.equal(statSync(path + ".tmp").mode & 0o777, 0o600);
    assert.deepEqual(json(path), { keep: 1 });
    writeFileSync(release, "go");
    assert.equal(await ended, 0, stderr);
    assert.ok(json(path)["openai-codex"]);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
