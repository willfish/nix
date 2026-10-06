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
  readlinkSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import test from "node:test";
const bin = process.env.GREETER_SELECT_BIN!,
  fixture = process.env.GREETER_SELECT_FIXTURE!,
  failure = process.env.GREETER_SELECT_FAILURES!,
  legacy = process.env.GREETER_SELECT_LEGACY,
  python = process.env.GREETER_SELECT_PYTHON || "python3";
assert.ok(
  bin && fixture && failure,
  "Set binary, fixture and failure-library paths",
);
const store = "/nix/store/disposable-greeter-",
  fallback = store + "fallback";
function disposable(fn: (path: string) => void) {
  const path = mkdtempSync(join(tmpdir(), "greeter-select-"));
  try {
    fn(path);
  } finally {
    rmSync(path, { recursive: true, force: true });
  }
}
function setup(path: string) {
  const selection = join(path, "selection"),
    runtime = join(path, "runtime");
  mkdirSync(selection);
  mkdirSync(runtime, { mode: 0o755 });
  chmodSync(runtime, 0o755);
  return {
    fallback: "dark",
    selectionDir: selection,
    selectionName: "william",
    runtimeDir: runtime,
    themes: { dark: { path: fallback }, light: { path: store + "light" } },
  } as any;
}
function save(path: string, m: any) {
  const file = join(path, "manifest.json");
  writeFileSync(file, typeof m === "string" ? m : JSON.stringify(m));
  return file;
}
function run(file: string, old = false, options: any = {}) {
  const r = spawnSync(old ? python : bin, old ? [legacy!, file] : [file], {
    encoding: "utf8",
    timeout: 10000,
    ...options,
  });
  assert.ifError(r.error);
  assert.doesNotMatch(
    r.stderr,
    /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:|DEADLYSIGNAL/,
  );
  return r;
}
function tempNames(runtime: string) {
  return readdirSync(runtime).filter((x) => x.startsWith(".theme-"));
}
function fault(mode: string, extra: any = {}) {
  return {
    ...process.env,
    LD_PRELOAD: [process.env.GREETER_SELECT_ASAN_RT, failure]
      .filter(Boolean)
      .join(":"),
    GREETER_FIXTURE_FAIL: mode,
    ...extra,
  };
}
function checkSelection(
  data: Buffer | string | null,
  expected: string,
  status = 0,
) {
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path);
      if (data !== null) writeFileSync(join(m.selectionDir, "william"), data);
      const r = run(save(path, m), old);
      assert.equal(r.status, status, r.stderr);
      if (!status) {
        assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), expected);
        assert.equal(tempNames(m.runtimeDir).length, 0);
      }
    });
}

test("selected allowlisted IDs, absent/unknown IDs and exact null entries resolve to the existing fallback", () => {
  checkSelection("light\n", store + "light");
  checkSelection("light", store + "light");
  checkSelection(null, fallback);
  checkSelection("unknown\n", fallback);
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path);
      m.themes.light = null;
      writeFileSync(join(m.selectionDir, "william"), "light");
      assert.equal(run(save(path, m), old).status, 0);
      assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
    });
});
test("only lowercase ASCII alphanumeric segments and one optional final LF are accepted", () => {
  for (const data of [
    "",
    "\n",
    "LIGHT",
    " light",
    "light ",
    "light\r\n",
    "light\n\n",
    "light\nother",
    "-light",
    "light-",
    "light--name",
    "light_name",
    "light.name",
    "light\0",
    "lïght",
    "١",
    "ｌｉｇｈｔ",
    Buffer.from([255, 10]),
  ])
    checkSelection(data, fallback);
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path);
      m.themes["a-0-xyz9"] = { path: store + "valid" };
      writeFileSync(join(m.selectionDir, "william"), "a-0-xyz9\n");
      assert.equal(run(save(path, m), old).status, 0);
      assert.equal(
        readlinkSync(join(m.runtimeDir, "omarchy")),
        store + "valid",
      );
    });
});
test("64-byte read bound includes the optional LF and does not trust file metadata size", () => {
  for (const [length, newline, selected] of [
    [64, false, true],
    [63, true, true],
    [64, true, false],
    [65, false, false],
    [10000, false, false],
  ] as [number, boolean, boolean][])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path),
          id = "a".repeat(length);
        m.themes[id] = { path: store + "long" };
        writeFileSync(
          join(m.selectionDir, "william"),
          id + (newline ? "\n" : ""),
        );
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(
          readlinkSync(join(m.runtimeDir, "omarchy")),
          selected ? store + "long" : fallback,
        );
      });
});
test("symlink selection parents and leaves are refused instead of reading their targets", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const parent of [false, true])
      disposable((path) => {
        const m = setup(path);
        writeFileSync(join(m.selectionDir, "other"), "light");
        if (parent) {
          writeFileSync(join(m.selectionDir, "william"), "light");
          m.selectionDir = join(path, "alias");
          symlinkSync(join(path, "selection"), m.selectionDir);
        } else symlinkSync("other", join(m.selectionDir, "william"));
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
      });
});
test("nonregular selection leaves, FIFOs and non-directory or missing parents fall back without blocking", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const mode of ["directory", "fifo", "missing-parent", "file-parent"])
      disposable((path) => {
        const m = setup(path),
          leaf = join(m.selectionDir, "william");
        if (mode === "directory") mkdirSync(leaf);
        if (mode === "fifo") {
          const r = spawnSync("mkfifo", [leaf]);
          assert.equal(r.status, 0);
        }
        if (mode === "missing-parent") m.selectionDir = join(path, "missing");
        if (mode === "file-parent") {
          writeFileSync(join(path, "file"), "light");
          m.selectionDir = join(path, "file");
        }
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
      });
});
test("trusted manifest relative/absolute selection names retain openat path semantics", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const name of ["nested/theme", "../external", "absolute"])
      disposable((path) => {
        const m = setup(path);
        mkdirSync(join(m.selectionDir, "nested"));
        const actual =
          name === "absolute"
            ? join(path, "absolute")
            : join(m.selectionDir, name);
        writeFileSync(actual, "light");
        m.selectionName = name === "absolute" ? actual : name;
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(
          readlinkSync(join(m.runtimeDir, "omarchy")),
          store + "light",
        );
      });
});
test("an absent fallback fails only when needed, including null selected entries", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const selected of [true, false])
      disposable((path) => {
        const m = setup(path);
        m.fallback = "missing";
        writeFileSync(
          join(m.selectionDir, "william"),
          selected ? "light" : "unknown",
        );
        const r = run(save(path, m), old);
        assert.equal(r.status, selected ? 0 : 1);
        if (!selected) {
          assert.match(r.stderr, /greeter fallback is not in the allowlist/);
          assert.equal(existsSync(join(m.runtimeDir, "omarchy")), false);
        }
      });
});
test("store allowlist rejects nonstrings, LF, NUL, wrong prefixes and exact dot-dot components", () => {
  for (const target of [
    null,
    false,
    17,
    {},
    "/tmp/theme",
    "/nix/store",
    "/nix/storehouse/theme",
    "/nix/store/../evil",
    "/nix/store/a/../b",
    "/nix/store/a/..",
    "/nix/store/a\n",
    "/nix/store/a\0b",
  ])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path);
        m.themes.dark.path = target;
        const r = run(save(path, m), old);
        assert.equal(r.status, 1);
        assert.match(r.stderr, /refusing non-store theme asset/);
        assert.equal(readdirSync(m.runtimeDir).length, 0);
      });
});
test("allowed store spellings are not realpath-normalized or required to exist", () => {
  for (const target of [
    "/nix/store/",
    "/nix/store/a/./b",
    "/nix/store/a//b",
    "/nix/store/a/..b",
    "/nix/store/a\rb",
    "/nix/store/a/尾🙂",
    "/nix/store/a/",
  ])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path);
        m.themes.dark.path = target;
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), target);
      });
});
test("selected malformed entries fail rather than choosing a different theme; unused entries stay unused", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const entry of [false, [], 17, {}, { path: "/tmp/bad" }])
      disposable((path) => {
        const m = setup(path);
        m.themes.light = entry;
        writeFileSync(join(m.selectionDir, "william"), "light");
        assert.equal(run(save(path, m), old).status, 1);
        assert.equal(readdirSync(m.runtimeDir).length, 0);
        writeFileSync(join(m.selectionDir, "william"), "dark");
        assert.equal(run(save(path, m), old).status, 0);
      });
});
test("runtime symlinks, files, missing paths and group/world-writable directories are rejected", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const mode of ["symlink", "file", "missing", "group", "world"])
      disposable((path) => {
        const m = setup(path);
        if (["symlink", "file", "missing"].includes(mode)) {
          rmSync(m.runtimeDir, { recursive: true });
          if (mode === "symlink") symlinkSync(m.selectionDir, m.runtimeDir);
          if (mode === "file") writeFileSync(m.runtimeDir, "keep");
        } else chmodSync(m.runtimeDir, mode === "group" ? 0o775 : 0o757);
        const r = run(save(path, m), old);
        assert.equal(r.status, 1);
        if (mode !== "missing")
          assert.match(
            r.stderr,
            /refusing (?:unowned|writable) runtime directory/,
          );
        if (mode === "file")
          assert.equal(readFileSync(m.runtimeDir, "utf8"), "keep");
      });
});
test("runtime owner must equal effective UID", () => {
  disposable((path) => {
    const m = setup(path);
    for (const old of legacy ? [false, true] : [false]) {
      const r = run(save(path, m), old, { env: fault("uid") });
      assert.equal(r.status, 1, r.stderr);
      assert.match(r.stderr, /refusing unowned runtime directory/);
      assert.equal(readdirSync(m.runtimeDir).length, 0);
    }
  });
});
test("existing runtime permissions remain unchanged and trusted ancestor links keep existing behavior", () => {
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path);
      chmodSync(m.runtimeDir, 0o750);
      symlinkSync(path, join(path, "ancestor"));
      m.runtimeDir = join(path, "ancestor/runtime");
      assert.equal(run(save(path, m), old).status, 0);
      assert.equal(statSync(m.runtimeDir).mode & 0o777, 0o750);
      assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
    });
});
test("atomic publication replaces leaf symlinks and regular files without editing their targets", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const type of ["symlink", "file"])
      disposable((path) => {
        const m = setup(path),
          out = join(m.runtimeDir, "omarchy"),
          other = join(path, "other");
        writeFileSync(other, "keep");
        if (type === "symlink") symlinkSync(other, out);
        else writeFileSync(out, "old");
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(lstatSync(out).isSymbolicLink(), true);
        assert.equal(readlinkSync(out), fallback);
        assert.equal(readFileSync(other, "utf8"), "keep");
        assert.equal(tempNames(m.runtimeDir).length, 0);
      });
});
test("an existing output directory makes rename fail without changing it or leaking temporary links", () => {
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path),
        out = join(m.runtimeDir, "omarchy");
      mkdirSync(out);
      writeFileSync(join(out, "keep"), "old");
      assert.equal(run(save(path, m), old).status, 1);
      assert.equal(readFileSync(join(out, "keep"), "utf8"), "old");
      assert.equal(tempNames(m.runtimeDir).length, 0);
    });
});
test("duplicate manifest keys use the final value, including byte-length-aware NUL and Unicode fallback keys", () => {
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path);
      const text = JSON.stringify(m)
        .replace('"fallback":"dark"', '"fallback":"wrong","fallback":"dark"')
        .replace(
          '"path":"' + fallback + '"',
          '"path":"/tmp/wrong","path":"' + fallback + '"',
        );
      assert.equal(run(save(path, text), old).status, 0);
      assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
    });
  for (const key of ["nul\0key", "尾🙂", "\udcff"])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path);
        m.fallback = key;
        m.themes[key] = { path: store + "key" };
        assert.equal(run(save(path, m), old).status, 0);
        assert.equal(
          readlinkSync(join(m.runtimeDir, "omarchy")),
          store + "key",
        );
      });
});
test("strict UTF-8 and JSON syntax failures happen before runtime mutation", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const text of [
      "\ufeff{}",
      "{} trailing",
      "{/*comment*/}",
      '{"a":1,}',
      '{"a":"\\q"}',
      '{"a":"raw\nline"}',
      "null",
      "[]",
      "{}",
    ])
      disposable((path) => {
        const m = setup(path),
          out = join(m.runtimeDir, "omarchy");
        symlinkSync("/old", out);
        assert.equal(run(save(path, text), old).status, 1);
        assert.equal(readlinkSync(out), "/old");
      });
  for (const old of legacy ? [false, true] : [false])
    disposable((path) => {
      const m = setup(path),
        file = save(path, m);
      writeFileSync(file, Buffer.from([0xff]));
      assert.equal(run(file, old).status, 1);
      assert.equal(readdirSync(m.runtimeDir).length, 0);
    });
});
test("unused JSON values retain Python constants, large integers and escaped-surrogate behavior", () => {
  for (const token of [
    "NaN",
    "Infinity",
    "-Infinity",
    "1e999",
    "-1e999",
    "1".repeat(100),
    '"\\ud800"',
    '"\\ud800\\udc80"',
    '"\\ud800\\u0041\\udcff"',
    '"\\\\ud800"',
  ])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path),
          text = JSON.stringify(m).replace(
            '"fallback"',
            '"unused":' + token + ',"fallback"',
          );
        assert.equal(run(save(path, text), old).status, 0);
      });
  for (const token of [
    "Inf",
    "nan",
    "NAN",
    "infinity",
    "-NaN",
    "01",
    "+1",
    "1".repeat(4301),
  ])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path),
          text = JSON.stringify(m).replace(
            '"fallback"',
            '"unused":' + token + ',"fallback"',
          );
        assert.equal(run(save(path, text), old).status, 1);
        assert.equal(readdirSync(m.runtimeDir).length, 0);
      });
});
test("filesystem surrogateescape preserves raw target bytes while other lone surrogates fail safely", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const suffix of ["\udcff", "\ud800", "\udc00"])
      disposable((path) => {
        const m = setup(path);
        m.themes.dark.path = store + suffix;
        const r = run(save(path, m), old);
        assert.equal(r.status, suffix === "\udcff" ? 0 : 1, r.stderr);
        if (!r.status)
          assert.deepEqual(
            readlinkSync(join(m.runtimeDir, "omarchy"), { encoding: "buffer" }),
            Buffer.concat([Buffer.from(store), Buffer.from([255])]),
          );
        assert.equal(tempNames(m.runtimeDir).length, 0);
      });
});
test("NUL in filesystem fields fails rather than opening a truncated path", () => {
  for (const field of ["selectionDir", "selectionName", "runtimeDir"])
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const m = setup(path);
        m[field] += "\0suffix";
        assert.equal(run(save(path, m), old).status, 1);
        assert.equal(readdirSync(join(path, "runtime")).length, 0);
      });
});
test("selection fstat/read/close failures remain errors, not fallback publications", () => {
  for (const mode of ["stat", "read", "close"])
    disposable((path) => {
      const m = setup(path),
        leaf = join(m.selectionDir, "william");
      writeFileSync(leaf, "light");
      const r = run(save(path, m), false, {
        env: fault(mode, { GREETER_FIXTURE_SELECTION: leaf }),
      });
      assert.equal(r.status, 1, r.stderr);
      assert.equal(readdirSync(m.runtimeDir).length, 0);
    });
});
test("a single short regular-file read retains the original bounded-read result", () => {
  disposable((path) => {
    const m = setup(path),
      leaf = join(m.selectionDir, "william");
    writeFileSync(leaf, "light");
    m.themes.li = { path: store + "short" };
    assert.equal(
      run(save(path, m), false, {
        env: fault("short-read", { GREETER_FIXTURE_SELECTION: leaf }),
      }).status,
      0,
    );
    assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), store + "short");
  });
});
test("publication failures retain the old link and clean only successfully created private temporaries", () => {
  for (const mode of ["create", "symlink", "rename"])
    disposable((path) => {
      const m = setup(path),
        out = join(m.runtimeDir, "omarchy");
      symlinkSync("/old", out);
      const existing = join(m.runtimeDir, ".theme-ownedx");
      if (mode === "create") {
        mkdirSync(existing);
        writeFileSync(join(existing, "keep"), "unowned");
      }
      assert.equal(run(save(path, m), false, { env: fault(mode) }).status, 1);
      assert.equal(readlinkSync(out), "/old");
      if (mode === "create")
        assert.equal(readFileSync(join(existing, "keep"), "utf8"), "unowned");
      else assert.equal(tempNames(m.runtimeDir).length, 0);
    });
});
test("cleanup failure after publication does not roll back the successful link", () => {
  disposable((path) => {
    const m = setup(path),
      out = join(m.runtimeDir, "omarchy");
    symlinkSync("/old", out);
    assert.equal(run(save(path, m), false, { env: fault("rmdir") }).status, 1);
    assert.equal(readlinkSync(out), fallback);
    const dirs = tempNames(m.runtimeDir);
    assert.equal(dirs.length, 1);
    assert.equal(readdirSync(join(m.runtimeDir, dirs[0])).length, 0);
    assert.equal(statSync(join(m.runtimeDir, dirs[0])).mode & 0o777, 0o700);
  });
});
test("publication waits behind a mode-0700 private directory and preserves the old link until rename", async () => {
  const path = mkdtempSync(join(tmpdir(), "greeter-publication-"));
  try {
    const m = setup(path),
      out = join(m.runtimeDir, "omarchy"),
      release = join(path, "release");
    symlinkSync("/old", out);
    const child = spawn(bin, [save(path, m)], {
      env: fault("", { GREETER_FIXTURE_RELEASE: release }),
      stdio: ["ignore", "ignore", "pipe"],
    });
    let stderr = "";
    child.stderr.on("data", (b) => (stderr += b));
    const ended = new Promise<number | null>((resolve) =>
      child.on("close", resolve),
    );
    let directory: string | undefined;
    for (let i = 0; i < 400 && !directory; i++) {
      directory = tempNames(m.runtimeDir)[0];
      if (!directory) await sleep(5);
    }
    assert.ok(directory);
    assert.equal(statSync(join(m.runtimeDir, directory)).mode & 0o777, 0o700);
    assert.equal(readlinkSync(out), "/old");
    assert.equal(
      readlinkSync(join(m.runtimeDir, directory, "omarchy")),
      fallback,
    );
    writeFileSync(release, "go");
    assert.equal(await ended, 0, stderr);
    assert.equal(readlinkSync(out), fallback);
    assert.equal(tempNames(m.runtimeDir).length, 0);
  } finally {
    rmSync(path, { recursive: true, force: true });
  }
});
test("direct native validation helpers preserve byte and component boundaries", () => {
  for (const [mode, items] of [
    [
      "id",
      [
        ["a", true],
        ["a-9\n", true],
        ["a\n\n", false],
        ["a\0b", false],
        ["a--b", false],
        ["a".repeat(100), true],
      ],
    ],
    [
      "store",
      [
        [store, true],
        ["/nix/store/", true],
        ["/nix/store/a/../b", false],
        ["/nix/store/a/..b", true],
        ["/nix/store/a\0b", false],
      ],
    ],
  ] as [string, [string, boolean][]][])
    for (const [value, expected] of items) {
      const r = spawnSync(fixture, [mode, Buffer.from(value).toString("hex")], {
        encoding: "utf8",
      });
      assert.equal(r.status, 0, r.stderr);
      assert.equal(r.stdout.trim(), String(expected));
    }
  disposable((path) => {
    const m = setup(path);
    writeFileSync(join(m.selectionDir, "william"), "light\n");
    const r = spawnSync(fixture, ["read", m.selectionDir, "william"], {
      encoding: "utf8",
    });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stdout, "light\n");
  });
});
test("raw invalid-UTF-8 manifest argv remains a filesystem byte path", () => {
  disposable((path) => {
    const m = setup(path),
      file = Buffer.concat([
        Buffer.from(path + "/manifest-"),
        Buffer.from([255]),
      ]);
    writeFileSync(file, JSON.stringify(m));
    for (const old of legacy ? [false, true] : [false]) {
      const r = spawnSync(
        fixture,
        old
          ? ["raw", file.toString("hex"), python, legacy!]
          : ["raw", file.toString("hex"), bin],
        { encoding: "utf8" },
      );
      assert.equal(r.status, 0, r.stderr);
      assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
    }
  });
});
test(
  "every generated catalogue entry publishes its exact declared asset without reading live assets",
  { skip: !process.env.GREETER_SELECT_MANIFEST },
  () => {
    const original = JSON.parse(
      readFileSync(process.env.GREETER_SELECT_MANIFEST!, "utf8"),
    );
    const ids = Object.keys(original.themes);
    assert.ok(ids.length > 0);
    for (const old of legacy ? [false, true] : [false])
      disposable((path) => {
        const local = setup(path);
        const m = {
          ...original,
          selectionDir: local.selectionDir,
          selectionName: local.selectionName,
          runtimeDir: local.runtimeDir,
        };
        const file = save(path, m);
        for (const id of ids) {
          writeFileSync(join(m.selectionDir, m.selectionName), id + "\n");
          const r = run(file, old);
          assert.equal(r.status, 0, r.stderr);
          assert.equal(
            readlinkSync(join(m.runtimeDir, "omarchy")),
            original.themes[id].path,
          );
          assert.equal(tempNames(m.runtimeDir).length, 0);
        }
      });
  },
);
test(
  "evaluated graphical-host services retain authentication, confinement, ordering and tmpfiles",
  { skip: !process.env.GREETER_SELECT_WIRING },
  () => {
    const wiring = JSON.parse(
      readFileSync(process.env.GREETER_SELECT_WIRING!, "utf8"),
    );
    assert.equal(wiring.terminus, null);
    for (const host of ["foundation", "andromeda", "starfish"]) {
      const c = wiring[host],
        s = c.serviceConfig;
      assert.equal(c.autoLogin, false);
      assert.equal(c.sddm.Autologin, undefined);
      assert.equal(
        c.sddm.General.GreeterEnvironment,
        "QML_DISABLE_DISK_CACHE=1",
      );
      assert.equal(s.Type, "oneshot");
      assert.equal(s.User, "root");
      assert.equal(s.UMask, "0022");
      assert.equal(s.NoNewPrivileges, true);
      assert.equal(s.PrivateTmp, true);
      assert.equal(s.ProtectHome, true);
      assert.equal(s.ProtectSystem, "strict");
      assert.deepEqual(s.ReadWritePaths, ["/run/desktop-login"]);
      assert.deepEqual(s.RestrictAddressFamilies, ["AF_UNIX"]);
      assert.match(
        s.ExecStart,
        /\/bin\/greeter-select \/nix\/store\/[^ ]+-sddm-themes\.json$/,
      );
      assert.doesNotMatch(s.ExecStart, /python/);
      assert.ok(c.after.includes("systemd-tmpfiles-setup.service"));
      assert.ok(c.before.includes("display-manager.service"));
      assert.equal(c.requiresMountsFor, "/var/lib/desktop-theme");
      assert.equal(c.path.PathChanged, "/var/lib/desktop-theme/william");
      assert.equal(c.path.Unit, "desktop-login-theme.service");
      assert.ok(
        c.displayManager.requires.includes("desktop-login-theme.service"),
      );
      assert.ok(c.displayManager.after.includes("desktop-login-theme.service"));
      assert.equal(c.tmpfiles["/run/desktop-login"].d.user, "root");
      assert.equal(c.tmpfiles["/run/desktop-login"].d.mode, "0755");
      assert.equal(c.tmpfiles["/var/lib/desktop-theme"].d.user, "root");
      assert.equal(
        c.tmpfiles["/var/lib/desktop-theme/william"].f.user,
        "william",
      );
      assert.equal(c.tmpfiles["/var/lib/desktop-theme/william"].f.mode, "0644");
    }
  },
);
test("CLI help, option terminator, argument errors and negative-looking manifest filenames retain argparse behavior", () => {
  for (const old of legacy ? [false, true] : [false]) {
    for (const args of [
      [],
      ["one", "two"],
      ["--unknown"],
      ["--help=bad"],
      ["--h=x", "-h"],
      ["-h=foo", "-h"],
    ]) {
      const r = spawnSync(old ? python : bin, old ? [legacy!, ...args] : args, {
        encoding: "utf8",
      });
      assert.equal(r.status, 2);
    }
    for (const args of [
      ["-h"],
      ["--help"],
      ["--h"],
      ["one", "two", "-h"],
      ["-hh"],
      ["-hfoo"],
      ["--unknown", "-h"],
    ]) {
      const r = spawnSync(old ? python : bin, old ? [legacy!, ...args] : args, {
        encoding: "utf8",
      });
      assert.equal(r.status, 0);
      assert.match(r.stdout, /manifest/);
    }
  }
  for (const old of legacy ? [false, true] : [false])
    for (const name of ["--help", "-1", "-.5", "-١"])
      disposable((path) => {
        const m = setup(path);
        writeFileSync(join(path, name), JSON.stringify(m));
        const r = spawnSync(
          old ? python : bin,
          old
            ? [legacy!, ...(name === "--help" ? ["--"] : []), name]
            : [...(name === "--help" ? ["--"] : []), name],
          { cwd: path, encoding: "utf8" },
        );
        assert.equal(r.status, 0, r.stderr);
        assert.equal(readlinkSync(join(m.runtimeDir, "omarchy")), fallback);
      });
});
