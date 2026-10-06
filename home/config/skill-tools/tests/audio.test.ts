import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { after, test } from "node:test";
import { tmpdir } from "node:os";
import { join } from "node:path";
const root = fs.realpathSync(fs.mkdtempSync(join(tmpdir(), "audio-tools-")));
after(() => fs.rmSync(root, { recursive: true, force: true }));
let serial = 0;
const bin = process.env.SKILL_TOOLS_BIN!;
assert(bin);
const inventory =
    process.env.INVENTORY_BIN || join(bin, "qbittorrent-inventory"),
  duplicate =
    process.env.DUPLICATE_BIN || join(bin, "source-target-duplicate-check");
function dir() {
  const path = join(root, String(serial++));
  fs.mkdirSync(path);
  return path;
}
function bencode(value: any): Buffer {
  if (Buffer.isBuffer(value))
    return Buffer.concat([Buffer.from(`${value.length}:`), value]);
  if (typeof value === "string") return bencode(Buffer.from(value));
  if (typeof value === "number" || typeof value === "bigint")
    return Buffer.from(`i${value}e`);
  if (Array.isArray(value))
    return Buffer.concat([
      Buffer.from("l"),
      ...value.map(bencode),
      Buffer.from("e"),
    ]);
  return Buffer.concat([
    Buffer.from("d"),
    ...Object.entries(value).flatMap(([k, v]) => [bencode(k), bencode(v)]),
    Buffer.from("e"),
  ]);
}
function call(command: string, args: string[], input?: string, parity = true) {
  const r = spawnSync(command, args, { input, timeout: 10000 });
  assert.notEqual(r.status, null, r.stderr.toString());
  assert(!r.stderr.toString().includes("Sanitizer"), r.stderr.toString());
  const legacy =
    command === inventory
      ? process.env.INVENTORY_LEGACY
      : process.env.DUPLICATE_LEGACY;
  if (legacy && parity) {
    const old = spawnSync(
      process.env.LEGACY_PYTHON || "python3",
      [legacy, ...args],
      { input, timeout: 10000 },
    );
    assert.equal(r.status, old.status, `${r.stderr}\n${old.stderr}`);
    if (args.includes("json") && r.stdout.length)
      assert.deepEqual(
        JSON.parse(r.stdout.toString()),
        JSON.parse(old.stdout.toString()),
      );
    else if (r.status === 0 || (command === duplicate && r.stdout.length))
      assert.deepEqual(r.stdout, old.stdout);
  }
  return r;
}
function inv() {
  const downloads = dir(),
    resume = dir();
  const args = ["--download-root", downloads, "--fastresume-dir", resume];
  return {
    downloads,
    resume,
    args,
    add(name: string | Buffer, data: any = {}, file?: string) {
      const key = file || `${serial++}.fastresume`;
      fs.writeFileSync(
        join(resume, key),
        bencode({
          name,
          "qBt-savePath": downloads,
          finished_time: 1,
          total_downloaded: 10,
          ...data,
        }),
      );
    },
  };
}
function report(command: string, args: string[], input?: string) {
  const r = call(command, [...args, "--format", "json"], input);
  return { code: r.status, value: JSON.parse(r.stdout.toString()) };
}
function dup(names: string) {
  const library = dir(),
    sourceRoot = dir(),
    sources = join(dir(), "names");
  fs.writeFileSync(sources, names);
  return {
    library,
    sourceRoot,
    sources,
    args: ["--sources-file", sources, "--targets", library],
  };
}
test("inventory statuses, bit padding, primary finished-time and transfer-ready filtering", () => {
  const f = inv();
  for (const [name, data, status] of [
    ["complete.m4b", {}, "complete"],
    [
      "pieces.mp3",
      { finished_time: 0, pieces: Buffer.from([255, 255]) },
      "complete",
    ],
    [
      "partial",
      { finished_time: 0, pieces: Buffer.from([0, 255]) },
      "incomplete",
    ],
    [
      "padding",
      { finished_time: 0, pieces: Buffer.from([255, 128]) },
      "unknown",
    ],
    ["empty", { finished_time: 0, total_downloaded: 0 }, "incomplete"],
    ["missing.m4b", {}, "missing_path"],
  ] as any[]) {
    if (name !== "missing.m4b")
      fs.writeFileSync(join(f.downloads, name), "payload");
    f.add(name, data);
    const p = report(inventory, f.args).value;
    assert.equal(p.at(-1).status, status);
  }
  assert.equal(
    report(inventory, [...f.args, "--transfer-ready-only"]).value.length,
    2,
  );
  assert.equal(call(inventory, [...f.args, "--format", "markdown"]).status, 0);
});
test("inventory never walks unregistered download roots and filters audiobook names and nested media", () => {
  const f = inv();
  fs.writeFileSync(join(f.downloads, "not registered.m4b"), "private");
  for (const name of ["Course", "Unabridged story", "text.txt"]) {
    fs.mkdirSync(join(f.downloads, name));
    f.add(name);
  }
  fs.writeFileSync(join(f.downloads, "Course", "01.MP3"), "123");
  fs.writeFileSync(join(f.downloads, "text.txt", "notes"), "12345");
  fs.writeFileSync(join(f.downloads, ".mp3"), "media");
  f.add(".mp3");
  const p = report(inventory, [...f.args, "--audiobook-only"]).value;
  assert.deepEqual(
    p.map((r: any) => r.name),
    ["Course", "Unabridged story", ".mp3"],
  );
  assert.equal(p[0].size_bytes, 3);
  assert.equal(p[1].size_bytes, 0);
});
test("unsafe names, parent traversal and payload symlink escapes are excluded", () => {
  const f = inv(),
    outside = dir();
  fs.writeFileSync(join(outside, "secret.m4b"), "do not include");
  fs.symlinkSync(outside, join(f.downloads, "escape"));
  for (const name of [
    "/absolute",
    "../secret.m4b",
    "inside/../../outside",
    "\\absolute",
    "escape/secret.m4b",
  ])
    f.add(name);
  assert.deepEqual(report(inventory, f.args).value, []);
  fs.mkdirSync(join(f.downloads, "inside"));
  fs.writeFileSync(join(f.downloads, "inside", "book.m4b"), "ok");
  f.add("./inside//book.m4b");
  assert.equal(report(inventory, f.args).value.length, 1);
});
test("resume aliases, fallback save path, integer coercion, malformed bencode and trailing junk", () => {
  const f = inv();
  fs.writeFileSync(join(f.downloads, "book.m4b"), "12345");
  fs.writeFileSync(
    join(f.resume, "a.fastresume"),
    Buffer.concat([
      bencode({
        "qBt-name": "book.m4b",
        finished_time: " +123 ",
        paused: "-1",
        total_downloaded: "invalid",
      }),
      Buffer.from("trailing junk"),
    ]),
  );
  for (const data of [
    "garbage",
    "d4:name100:x",
    "di1e1:xe",
    "l1:xe",
    "d4:namei0ee",
  ])
    fs.writeFileSync(join(f.resume, `${serial++}.fastresume`), data);
  const p = report(inventory, f.args).value;
  assert.equal(p.length, 1);
  assert.equal(p[0].paused, true);
  assert.equal(p[0].finished_time, 123);
  assert.equal(p[0].total_downloaded, 0);
  assert.equal(p[0].save_path, f.downloads);
});
test("byte-safe NUL transfer lists preserve newlines and surrogateescaped filenames", () => {
  const f = inv();
  const raw = Buffer.from([0x62, 0xff, 0x2e, 0x6d, 0x34, 0x62]);
  fs.writeFileSync(
    Buffer.concat([Buffer.from(f.downloads + "/"), raw]),
    "data",
  );
  f.add(raw, {}, "a.fastresume");
  const name = "book\nsecond line.m4b";
  fs.writeFileSync(join(f.downloads, name), "data");
  f.add(name, {}, "b.fastresume");
  let r = call(inventory, [
    ...f.args,
    "--transfer-ready-only",
    "--format",
    "nul",
  ]);
  assert.deepEqual(
    r.stdout,
    Buffer.concat([raw, Buffer.from("\0" + name + "\0")]),
  );
  const p = report(inventory, f.args).value;
  assert.equal(p[0].name, "b\udcff.m4b");
  const output = join(dir(), "list");
  r = call(inventory, [...f.args, "--format", "nul", "-o", output]);
  assert.equal(r.status, 0);
  assert.equal(r.stdout.length, 0);
  assert(fs.readFileSync(output).includes(0));
});
test("inventory missing resume directories are empty and output errors are nonzero", () => {
  const f = inv();
  assert.deepEqual(
    report(inventory, ["--fastresume-dir", join(root, "missing")]).value,
    [],
  );
  assert.equal(
    call(inventory, [...f.args, "--format", "names"]).stdout.length,
    0,
  );
  assert.equal(
    call(inventory, [...f.args, "-o", join(root, "absent", "out")]).status,
    1,
  );
  assert.equal(call(inventory, ["--format", "bad"]).status, 2);
});
test("duplicate match precedence covers exact, normalized, ASIN and size hints", () => {
  const f = dup(
    "Exact\nNormal Book (Unabridged).m4b\nElse [B012345678]\nsize source\nClean\n",
  );
  fs.mkdirSync(join(f.library, "Exact"));
  fs.mkdirSync(join(f.library, "Normal Book"));
  fs.mkdirSync(join(f.library, "Target [B012345678]"));
  fs.writeFileSync(join(f.library, "other.mp3"), "1234567");
  fs.writeFileSync(join(f.sourceRoot, "size source"), "1234567");
  const p = report(duplicate, [...f.args, "--source-root", f.sourceRoot]).value;
  assert.equal(p.status, "complete");
  assert.deepEqual(
    p.matches.map((m: any) => m.match_type),
    ["exact_name", "normalized", "asin", "size"],
  );
  assert.equal(p.sources[3].size_bytes, 7);
  const r = call(duplicate, [...f.args, "--source-root", f.sourceRoot]);
  assert(
    r.stdout.toString().includes("No hit (eligible to stage pending full AC5)"),
  );
});
test("duplicate normalization removes noise, audio extensions and ASIN brackets", () => {
  const f = dup(
    "The Book Complete WEBRip x264 720p NF GalaxyTV TGx.m4b\nOther [B087654321] (abridged).cue\n",
  );
  fs.mkdirSync(join(f.library, "The Book"));
  fs.mkdirSync(join(f.library, "Other"));
  const p = report(duplicate, f.args).value;
  assert.equal(p.match_count, 2);
  assert(p.matches.every((m: any) => m.match_type === "normalized"));
});
test("missing roots, non-directories and mixed successful scans remain incomplete", () => {
  const f = dup("Book\n");
  fs.mkdirSync(join(f.library, "Book"));
  const missing = join(root, "missing-one"),
    second = join(root, "missing-two");
  const args = [
    "--sources-file",
    f.sources,
    "--targets",
    f.library,
    missing,
    second,
    f.sources,
  ];
  const p = report(duplicate, args);
  assert.equal(p.code, 1);
  assert.equal(p.value.status, "incomplete");
  assert.equal(p.value.scan_errors.length, 3);
  assert.equal(p.value.match_count, null);
  assert.deepEqual(p.value.matches, []);
  const r = call(duplicate, args);
  assert(!r.stdout.toString().includes("eligible to stage"));
  const output = join(dir(), "incomplete.json");
  assert.equal(
    call(duplicate, [...args, "--format", "json", "-o", output]).status,
    1,
  );
  assert.equal(
    JSON.parse(fs.readFileSync(output, "utf8")).status,
    "incomplete",
  );
});
test("permission failures and broken links fail closed instead of reporting a clean scan", () => {
  assert.notEqual(process.getuid!(), 0);
  const f = dup("Book\n"),
    locked = join(f.library, "locked");
  fs.mkdirSync(locked, 0o000);
  try {
    const p = report(duplicate, f.args);
    assert.equal(p.code, 1);
    assert.equal(p.value.match_count, null);
  } finally {
    fs.chmodSync(locked, 0o700);
  }
  fs.symlinkSync("/missing/target", join(f.library, "broken"));
  assert.equal(report(duplicate, f.args).code, 1);
});
test("target depth, file suffix rules, symlink traversal and canonical path deduplication", () => {
  const f = dup("Book\nonlyaac.aac\nonlywav.wav\ntrack.mp3\n");
  const author = join(f.library, "Author");
  fs.mkdirSync(author);
  fs.mkdirSync(join(author, "Book"));
  for (const name of ["onlyaac.aac", "onlywav.wav", "track.mp3"])
    fs.writeFileSync(join(author, name), "data");
  let p = report(duplicate, [...f.args, "--max-depth", "1"]).value;
  assert.equal(p.match_count, 0);
  p = report(duplicate, [...f.args, "--max-depth", "2"]).value;
  assert.equal(p.match_count, 2);
  fs.symlinkSync(author, join(f.library, "Alias"));
  p = report(duplicate, [...f.args, "--targets", f.library, f.library]).value;
  assert.equal(p.match_count, 2);
  assert.equal(call(duplicate, [...f.args, "--max-depth", "0"]).status, 2);
});
test("Libation inventory reads only the index and emits present direct/nested media paths", () => {
  const home = dir(),
    index = join(home, ".local/share/Libation/FileLocationsV2.json");
  fs.mkdirSync(join(home, ".local/share/Libation"), { recursive: true });
  const media = join(home, "book.m4b");
  fs.writeFileSync(media, "media");
  fs.writeFileSync(
    index,
    JSON.stringify({
      Dictionary: {
        B012345678: [
          { Path: { Path: media } },
          { Path: media },
          { Path: "/missing/book" },
          { Path: null },
          { Path: 0 },
          { Path: [] },
          { Path: {} },
        ],
      },
    }),
  );
  fs.writeFileSync(
    join(home, ".local/share/Libation/AccountsSettings.json"),
    "not JSON; never read this",
  );
  const command = process.env.LIBATION_BIN || join(bin, "libation-inventory");
  const result = spawnSync(command, [], {
    env: { ...process.env, HOME: home },
    encoding: "utf8",
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, media + "\n" + media + "\n");
  fs.writeFileSync(index, "{");
  assert.equal(spawnSync(command, [index]).status, 1);
  assert.equal(spawnSync(command, ["--help"]).status, 0);
});
test("sources stdin, Unicode whitespace, comments, repeated names and empty roots", () => {
  const library = dir();
  fs.mkdirSync(join(library, "Book"));
  const input = "\u0085# comment\n\u2003Book\u0085\nBook\nClean\n";
  const args = ["--sources-file", "-", "--targets", library];
  const p = report(duplicate, args, input).value;
  assert.equal(p.sources.length, 3);
  assert.equal(p.match_count, 2);
  assert.equal(call(duplicate, args, input).status, 0);
  const empty = dup("");
  assert.equal(report(duplicate, empty.args).value.match_count, 0);
});
