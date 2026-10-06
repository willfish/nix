import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import {
  chmodSync,
  existsSync,
  linkSync,
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

const bins = process.env.PANEL_SETTINGS_BIN_DIR!,
  legacy = process.env.PANEL_SETTINGS_LEGACY,
  python = process.env.PANEL_SETTINGS_PYTHON || "python3",
  failure = process.env.PANEL_SETTINGS_FAILURES!,
  raw = process.env.PANEL_SETTINGS_ARGV!;
assert.ok(
  bins && failure && raw,
  "Set panel binary, failure library and argv fixture paths",
);
const kinds = ["calendar", "weather"];
function root(fn: (path: string) => void) {
  const path = mkdtempSync(join(tmpdir(), "panel-settings-"));
  try {
    fn(path);
  } finally {
    rmSync(path, { recursive: true, force: true });
  }
}
function exe(kind: string) {
  return join(bins, "hypr-" + kind + "-settings");
}
function command(
  kind: string,
  dest: string,
  args: string[],
  old = false,
  options: any = {},
) {
  const r = spawnSync(
    old ? python : exe(kind),
    old ? [legacy!.replace("{kind}", kind), dest, ...args] : [dest, ...args],
    { encoding: "utf8", timeout: 15000, ...options },
  );
  assert.ifError(r.error);
  return r;
}
function compare(input: string, status = 0) {
  for (const kind of kinds)
    root((path) => {
      const a = join(path, "native/out"),
        b = join(path, "legacy/out");
      const r = command(kind, a, [input]);
      assert.equal(r.status, status, r.stderr);
      if (legacy) {
        const old = command(kind, b, [input], true);
        assert.equal(old.status, status, old.stderr);
        assert.equal(existsSync(a), existsSync(b));
        if (existsSync(a))
          assert.deepEqual(readFileSync(a), readFileSync(b), input);
      }
      return;
    });
}
function temps(path: string) {
  return readdirSync(path).filter((x) => /\.\w+-settings-/.test(x));
}
function failures(mode: string, extra: any = {}) {
  const preload = [process.env.PANEL_SETTINGS_ASAN_RT, failure]
    .filter(Boolean)
    .join(":");
  return {
    ...process.env,
    LD_PRELOAD: preload,
    PANEL_FIXTURE_FAIL: mode,
    ...extra,
  };
}

test("both public families accept complete nested objects and produce ASCII spaced JSON with LF", () => {
  for (const input of [
    "{}",
    '{"enabled":true,"nested":{"x":[1,false,null,"text"]}}',
    ' { "weather": {"city":"London","latitude":51.5074}, "colors": ["#fff", "dark"] } ',
    '{"quote":"\\\"\\\\/\\b\\f\\n\\r\\t","nul":"\\u0000","del":"\\u007f"}',
  ])
    compare(input);
});
test("duplicate keys keep first insertion order and the last value, recursively and with NUL keys", () => {
  for (const s of [
    '{"a":1,"b":2,"a":3}',
    '{"x":{"a":1,"a":2},"a":[{"z":1,"z":2}]}',
    '{"x\\u0000":1,"x":2,"x\\u0000":3}',
    '{"😀":1,"\\ud83d\\ude00":2}',
  ])
    compare(s);
});
test("Unicode, astral characters, lone surrogates and mixed escaped pairs retain exact encoding", () => {
  for (const s of [
    '{"text":"π🙂中é\u001f"}'.replace("\u001f", "\\u001f"),
    '{"x":"\\ud800","y":"\\udfff","z":"\\ud800x\\udc00"}',
    '{"x":"\\ud800\\udc00\\udbff\\udfff"}',
    '{"x":"\\ud800\\u0041\\udc00","y":"\\uFEFF"}',
    '{"x":"\u2028\u2029\u0085"}',
  ])
    compare(s);
});
test("arbitrary integers, negative integer zero and the decimal-digit conversion limit are preserved", () => {
  for (const n of [
    "-0",
    "18446744073709551615",
    "-9223372036854775809",
    "1" + "0".repeat(200),
    "1".repeat(4300),
  ])
    compare('{"x":' + n + "}");
  compare('{"x":' + "1".repeat(4301) + "}", 1);
});
test("real formatting preserves signed zero, shortest decimal and Python exponent thresholds", () => {
  const numbers = [
    "1.0",
    "-0.0",
    "-0e100",
    "0.1",
    "0.2",
    "1.2345678901234567",
    "1.0000000000000002",
    "1000000000000000.0",
    "10000000000000000.0",
    "1e-4",
    "1e-5",
    "0.00009999999999999999",
    "1e23",
    "1e100",
    "1e-300",
    "2.2250738585072014e-308",
    "5e-324",
    "1.7976931348623157e308",
    "-1e999",
    "1e999",
    "-1e-999",
    "1.234567890123456789e20",
  ];
  compare('{"x":[' + numbers.join(",") + "]}");
});
test("deterministic IEEE double corpus matches complete legacy output", () => {
  let seed = 17;
  const values = [];
  const buffer = Buffer.alloc(8);
  for (let i = 0; i < 1000; i++) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    buffer.writeUInt32LE(seed, 0);
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    buffer.writeUInt32LE(seed, 4);
    const x = buffer.readDoubleLE();
    if (Number.isFinite(x)) values.push(x.toExponential(17));
  }
  for (let i = 0; i < values.length; i += 40)
    compare('{"x":[' + values.slice(i, i + 40).join(",") + "]}");
});
test("NaN and infinities remain supported but extra nonstandard spellings are rejected", () => {
  compare('{"x":[NaN,Infinity,-Infinity,1e999,-1e999]}');
  for (const n of ["nan", "NAN", "Inf", "inf", "infinity", "-NaN", "+Infinity"])
    compare('{"x":' + n + "}", 1);
});
test("valid nonobjects have status 2, malformed input has status 1, without creating parents", () => {
  for (const s of ["null", "true", "false", "0", "1.2", '"x"', "[]", "[{}]"])
    compare(s, 2);
  for (const s of [
    "",
    " ",
    "\ufeff{}",
    "{",
    '{"x":1,}',
    '{"x":01}',
    '{"x":1.}',
    '{"x":.1}',
    '{"x":1e}',
    '{"x":+1}',
    '{"x":"\\q"}',
    '{"x":"line\nbreak"}',
    "/*x*/{}",
    "{} trailing",
    '{"x" 1}',
    "{false:1}",
  ])
    compare(s, 1);
  for (const kind of kinds)
    root((path) => {
      assert.equal(command(kind, join(path, "missing/out"), ["bad"]).status, 1);
      assert.equal(existsSync(join(path, "missing")), false);
    });
});
test("8,000-character boundary counts Unicode code points rather than UTF-8 bytes or UTF-16 units", () => {
  for (const text of ["a", "π", "🙂"]) {
    const payload = '{"x":"' + text.repeat(7992) + '"}';
    compare(payload);
    compare(payload + " ", 2);
  }
  for (const kind of kinds)
    root((path) => {
      const dest = join(path, "missing/out");
      for (const args of [
        [],
        ["{}", "{}"],
        ['{"x":"' + "a".repeat(7993) + '"}'],
      ])
        assert.equal(command(kind, dest, args).status, 2);
      assert.equal(existsSync(join(path, "missing")), false);
    });
});
test("serialization depth failures retain created parents without temporary files", () => {
  for (const [depth, leaf, status] of [
    [996, "0", 0],
    [997, "0", 1],
    [995, "[]", 0],
    [996, "[]", 1],
    [995, "0.5", 0],
    [996, "0.5", 1],
    [3900, "0", 1],
  ] as [number, string, number][])
    for (const kind of kinds)
      root((path) => {
        const payload =
            '{"x":' + "[".repeat(depth) + leaf + "]".repeat(depth) + "}",
          dest = join(path, "native/out");
        const r = command(kind, dest, [payload]);
        assert.equal(r.status, status, r.stderr);
        assert.equal(existsSync(join(path, "native")), true);
        assert.equal(temps(join(path, "native")).length, 0);
        if (legacy) {
          const old = join(path, "legacy/out"),
            r = command(kind, old, [payload], true);
          assert.equal(r.status, status, r.stderr);
          assert.equal(existsSync(join(path, "legacy")), true);
          if (!status) assert.deepEqual(readFileSync(dest), readFileSync(old));
        }
      });
});
test("encoding failure preserves an existing destination without leaked temporary files", () => {
  for (const kind of kinds)
    root((path) => {
      const dest = join(path, "out");
      writeFileSync(dest, "old");
      const ino = statSync(dest).ino;
      assert.equal(
        command(kind, dest, [
          '{"x":' + "[".repeat(997) + "0" + "]".repeat(997) + "}",
        ]).status,
        1,
      );
      assert.equal(readFileSync(dest, "utf8"), "old");
      assert.equal(statSync(dest).ino, ino);
      assert.equal(temps(path).length, 0);
    });
});
test("new leaf directories are private, intermediate directories follow umask, and existing modes remain", () => {
  for (const kind of kinds)
    root((path) => {
      const dest = join(path, "one/two/settings.json");
      assert.equal(command(kind, dest, ["{}"]).status, 0);
      assert.equal(statSync(join(path, "one/two")).mode & 0o777, 0o700);
      assert.equal(statSync(dest).mode & 0o777, 0o600);
      if (legacy) {
        const old = join(path, "old/one/two/settings.json");
        assert.equal(command(kind, old, ["{}"], true).status, 0);
        assert.equal(
          statSync(join(path, "one")).mode & 0o777,
          statSync(join(path, "old/one")).mode & 0o777,
        );
      }
      const parent = join(path, "existing");
      mkdirSync(parent, { mode: 0o755 });
      chmodSync(parent, 0o755);
      assert.equal(command(kind, join(parent, "out"), ["{}"]).status, 0);
      assert.equal(statSync(parent).mode & 0o777, 0o755);
    });
});
test("replacement changes inode, publishes mode 0600 and never edits an old symlink or hardlink target", () => {
  for (const kind of kinds)
    root((path) => {
      const target = join(path, "old"),
        dest = join(path, "out");
      writeFileSync(target, "keep");
      symlinkSync(target, dest);
      assert.equal(command(kind, dest, ['{"x":1}']).status, 0);
      assert.equal(lstatSync(dest).isSymbolicLink(), false);
      assert.equal(readFileSync(target, "utf8"), "keep");
      const hard = join(path, "hard");
      linkSync(target, hard);
      assert.equal(command(kind, hard, ["{}"]).status, 0);
      assert.equal(readFileSync(target, "utf8"), "keep");
      const ino = statSync(dest).ino;
      assert.equal(command(kind, dest, ['{"x":2}']).status, 0);
      assert.notEqual(statSync(dest).ino, ino);
      assert.equal(statSync(dest).mode & 0o777, 0o600);
    });
});
test("parent links and lexical dot/dot-dot paths retain existing filesystem behavior", () => {
  for (const kind of kinds)
    root((path) => {
      const actual = join(path, "actual");
      mkdirSync(actual);
      symlinkSync(actual, join(path, "link"));
      assert.equal(command(kind, join(path, "link/out"), ["{}"]).status, 0);
      assert.equal(readFileSync(join(actual, "out"), "utf8"), "{}\n");
      assert.equal(command(kind, path + "/missing/../out", ["{}"]).status, 0);
      assert.equal(existsSync(join(path, "missing")), true);
      assert.equal(readFileSync(join(path, "out"), "utf8"), "{}\n");
      assert.equal(command(kind, path + "/./actual//other", ["{}"]).status, 0);
    });
});
test("basename-only destination remains an error; directory and non-directory-parent failures retain old data", () => {
  for (const kind of kinds)
    root((path) => {
      assert.equal(
        command(kind, "settings", ["{}"], false, { cwd: path }).status,
        1,
      );
      assert.equal(existsSync(join(path, "settings")), false);
      const dir = join(path, "dir");
      mkdirSync(dir);
      assert.equal(command(kind, dir, ["{}"]).status, 1);
      assert.equal(temps(path).length, 0);
      writeFileSync(join(path, "file"), "keep");
      assert.equal(command(kind, join(path, "file/out"), ["{}"]).status, 1);
      assert.equal(readFileSync(join(path, "file"), "utf8"), "keep");
    });
});
test("failed write, chmod, close and rename preserve the destination and remove private temporaries", () => {
  for (const kind of kinds)
    for (const mode of ["write", "chmod", "close", "rename"])
      root((path) => {
        const dest = join(path, "out");
        writeFileSync(dest, "old");
        const ino = statSync(dest).ino;
        const r = command(kind, dest, ['{"x":1}'], false, {
          env: failures(mode),
        });
        assert.equal(r.status, 1, r.stderr);
        assert.equal(readFileSync(dest, "utf8"), "old");
        assert.equal(statSync(dest).ino, ino);
        assert.equal(temps(path).length, 0);
      });
});
test("cleanup touches only a successfully created temporary and never a reused name after success", () => {
  for (const kind of kinds)
    root((path) => {
      const existing = join(path, "." + kind + "-settings-ownedx");
      writeFileSync(existing, "unowned");
      assert.equal(
        command(kind, join(path, "out"), ["{}"], false, {
          env: failures("create"),
        }).status,
        1,
      );
      assert.equal(readFileSync(existing, "utf8"), "unowned");
      rmSync(existing);
      assert.equal(
        command(kind, join(path, "out"), ["{}"], false, {
          env: failures("post-rename"),
        }).status,
        0,
      );
      const temporary = temps(path);
      assert.equal(temporary.length, 1);
      assert.equal(readFileSync(join(path, temporary[0]), "utf8"), "other");
      assert.equal(readFileSync(join(path, "out"), "utf8"), "{}\n");
    });
});
test("short writes are completed rather than publishing partial JSON", () => {
  for (const kind of kinds)
    root((path) => {
      const dest = join(path, "out");
      const r = command(
        kind,
        dest,
        ['{"x":"' + "text".repeat(100) + '"}'],
        false,
        { env: failures("short-write") },
      );
      assert.equal(r.status, 0, r.stderr);
      assert.equal(
        JSON.parse(readFileSync(dest, "utf8")).x,
        "text".repeat(100),
      );
    });
});
test("temporary file stays private and the old file remains visible until atomic publication", async () => {
  for (const kind of kinds) {
    const path = mkdtempSync(join(tmpdir(), "panel-publication-"));
    try {
      const dest = join(path, "out"),
        release = join(path, "release");
      writeFileSync(dest, "old");
      const child = spawn(exe(kind), [dest, '{"x":1}'], {
        env: failures("", { PANEL_FIXTURE_RELEASE: release }),
        stdio: ["ignore", "ignore", "pipe"],
      });
      let stderr = "";
      child.stderr.on("data", (b) => (stderr += b));
      const ended = new Promise<number | null>((resolve) =>
        child.on("close", resolve),
      );
      let temporary: string | undefined;
      for (let i = 0; i < 400 && !temporary; i++) {
        temporary = temps(path)[0];
        if (!temporary) await sleep(5);
      }
      assert.ok(temporary);
      assert.equal(statSync(join(path, temporary)).mode & 0o777, 0o600);
      assert.equal(readFileSync(dest, "utf8"), "old");
      writeFileSync(release, "go");
      assert.equal(await ended, 0, stderr);
      assert.equal(readFileSync(dest, "utf8"), '{"x": 1}\n');
      assert.equal(temps(path).length, 0);
    } finally {
      rmSync(path, { recursive: true, force: true });
    }
  }
});
test("raw invalid UTF-8 argv is decoded with surrogateescape rather than replacement or rejection", () => {
  for (const kind of kinds)
    for (const mode of ["bad-byte", "surrogate", "bad-destination"])
      root((path) => {
        const a = join(path, "native"),
          b = join(path, "legacy");
        const r = spawnSync(raw, [mode, a, exe(kind)], { encoding: "utf8" });
        assert.equal(r.status, 0, r.stderr);
        const location =
          mode === "bad-destination"
            ? Buffer.concat([
                Buffer.from(a + "-"),
                Buffer.from([255]),
                Buffer.from("/settings.json"),
              ])
            : a;
        if (legacy) {
          const old = spawnSync(
            raw,
            [mode, b, python, legacy.replace("{kind}", kind)],
            { encoding: "utf8" },
          );
          assert.equal(old.status, 0, old.stderr);
          const target =
            mode === "bad-destination"
              ? Buffer.concat([
                  Buffer.from(b + "-"),
                  Buffer.from([255]),
                  Buffer.from("/settings.json"),
                ])
              : b;
          assert.deepEqual(readFileSync(location), readFileSync(target));
        }
      });
});
