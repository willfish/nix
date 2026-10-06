import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { after, test } from "node:test";
const root = fs.realpathSync(fs.mkdtempSync(join(tmpdir(), "skill-tools-")));
after(() => fs.rmSync(root, { recursive: true, force: true }));
const bin = process.env.SKILL_TOOLS_BIN!;
assert(bin);
const contrast = process.env.CONTRAST_BIN || join(bin, "contrast"),
  youtube = process.env.YOUTUBE_BIN || join(bin, "youtube-extract");
let serial = 0;
function file(name: string, value: string | Buffer) {
  const p = join(root, `${serial++}-${name}`);
  fs.writeFileSync(p, value);
  return p;
}
function run(program: string, args: string[], env: NodeJS.ProcessEnv = {}) {
  return spawnSync(program, args, {
    encoding: "utf8",
    env: { ...process.env, ...env },
    timeout: 10000,
  });
}
function check(
  program: string,
  args: string[],
  env: NodeJS.ProcessEnv = {},
  parity = true,
) {
  const r = run(program, args, env);
  assert.notEqual(r.status, null, r.stderr);
  assert(!r.stderr.includes("Sanitizer"), r.stderr);
  const legacy =
    program === contrast
      ? process.env.CONTRAST_LEGACY
      : process.env.YOUTUBE_LEGACY;
  if (legacy && parity) {
    const old = spawnSync(
      process.env.LEGACY_PYTHON || "python3",
      [legacy, ...args],
      { encoding: "utf8", env: { ...process.env, ...env }, timeout: 10000 },
    );
    assert.equal(r.status, old.status, `${r.stderr}\n${old.stderr}`);
    if (r.status === 0 || (program === contrast && r.status === 1)) {
      if (args.includes("--json"))
        assert.deepEqual(JSON.parse(r.stdout), JSON.parse(old.stdout));
      else assert.equal(r.stdout, old.stdout);
    }
  }
  return r;
}
const info = file(
  "info.json",
  JSON.stringify({
    id: "qILTuXLxfBM",
    title: "Example metadata",
    channel: "Example channel",
    duration: 1610,
    webpage_url: "https://www.youtube.com/watch?v=qILTuXLxfBM",
    description: "\u0085 A description\nwith two lines. \u2003",
    chapters: [
      { start_time: 214, title: "The model" },
      { start_time: 1051, title: "Settings" },
    ],
  }),
);
const vtt = file(
  "captions.vtt",
  `WEBVTT\nKind: captions\nLanguage: en\n\n00:03:34.080 --> 00:03:40.309\nat what I'm talking about. This is the\nQuen 3.8 27 billion GSQRCO GGUF. It's a\n\n00:03:40.319 --> 00:03:43.350\nQuen 3.8 27 billion GSQRCO GGUF. It's a\nnonuniform GGUF quantization produced\n\n00:17:31.000 --> 00:17:34.000\nthat I have the settings here and I got\nthe prompt library.\n\n00:17:34.000 --> 00:17:34.000\nthat I have the settings here and I got\nthe prompt library.\n`,
);
function payload(args: string[], env: NodeJS.ProcessEnv = {}) {
  const r = check(youtube, [...args, "--json"], env);
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
}
test("contrast black/white, identical colours and channel transfer/weighting", () => {
  for (const [a, b, ratio] of [
    ["#000000", "#ffffff", 21],
    ["#ffffff", "#000000", 21],
    ["#18212b", "#18212b", 1],
    ["#ff0000", "#000000", 5.252],
    ["#00ff00", "#000000", 15.304],
    ["#0000ff", "#000000", 2.444],
    ["#0a0a0a", "#000000", 1.060705],
    ["#0b0b0b", "#000000", 1.066931],
  ] as const) {
    const r = check(contrast, [a, b, "--minimum", "1"]);
    assert.equal(r.status, 0);
    assert(r.stdout.startsWith(`PASS ${ratio.toFixed(6)}:1`), r.stdout);
  }
});
test("contrast uses the unrounded ratio and preserves explicit thresholds", () => {
  assert.equal(check(contrast, ["#777777", "#ffffff"]).status, 1);
  assert.equal(check(contrast, ["#767676", "#ffffff"]).status, 0);
  assert.equal(
    check(contrast, ["#777777", "#ffffff", "--minimum", "4.478089463577213"])
      .status,
    1,
  );
  for (const minimum of ["3", "21", "1", " 4.5 ", "4_._5"]) {
    const r = check(contrast, ["#000000", "#ffffff", "--minimum", minimum]);
    assert.equal(r.status, minimum === "4_._5" ? 2 : 0);
  }
});
test("contrast rejects invalid colours, nonfinite thresholds and bad usage", () => {
  for (const c of [
    "white",
    "#fff",
    "#12345678",
    "#zz0000",
    "123456",
    " #000000",
  ])
    assert.equal(check(contrast, [c, "#ffffff"]).status, 2);
  for (const n of ["0", "22", "nan", "inf", "-inf", "banana", "0x10"])
    assert.equal(
      check(contrast, ["#000000", "#ffffff", "--minimum", n]).status,
      2,
    );
  assert.equal(check(contrast, []).status, 2);
  assert.equal(run(contrast, ["--help"]).status, 0);
});
test("contrast matches retained CLI across a deterministic colour sample", () => {
  let state = 1234567;
  const colour = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return "#" + (state & 0xffffff).toString(16).padStart(6, "0");
  };
  for (let i = 0; i < 40; i++) {
    const r = check(contrast, [colour(), colour()]);
    assert([0, 1].includes(r.status!));
  }
});
test("VTT metadata, rolling duplicate removal, around window and search", () => {
  const p = payload([
    "--info-file",
    info,
    "--subs-file",
    vtt,
    "--around",
    "214",
    "--query",
    "GGUF",
  ]);
  assert.equal(p.cue_count, 3);
  assert.equal(p.duration_string, "26:50");
  assert.equal(p.chapters[0].clock, "3:34");
  assert.equal(p.matches.length, 2);
  assert.equal(p.around.length, 2);
  assert.equal(p.timestamp, 214);
});
test("URL timestamp forms, explicit overrides and positional/flag precedence", () => {
  for (const [url, seconds] of [
    ["https://youtu.be/qILTuXLxfBM?t=17m31s", 1051],
    ["https://youtube.com/watch?v=qILTuXLxfBM&t=1051s", 1051],
    ["https://youtube.com/live/qILTuXLxfBM#t=1h2m3s", 3723],
    ["https://youtube.com/shorts/qILTuXLxfBM?t=1.5s", 1],
  ] as const) {
    assert.equal(payload([url, "--subs-file", vtt]).timestamp, seconds);
    assert.equal(
      payload([url, "--subs-file", vtt, "--around", "2.5"]).timestamp,
      2.5,
    );
  }
  assert.equal(
    payload([
      "https://youtu.be/qILTuXLxfBM?t=2",
      "--url",
      "https://youtu.be/qILTuXLxfBM?t=3",
      "--subs-file",
      vtt,
    ]).timestamp,
    3,
  );
  for (const value of ["1051", "1H2M3S", "17m31s", "١٢.٥", "\u0085 10 \u2003"])
    assert.equal(
      check(youtube, ["--subs-file", vtt, "--around", value, "--json"]).status,
      0,
    );
});
test("SRT, timestamp settings, CRLF, inclusive boundaries and Unicode casefold search", () => {
  const srt = file(
    "captions.srt",
    "1\r\n00:00:01,000 --> 00:00:02,500 align:start\r\n<i>Hello</i>&nbsp;Straße\r\n\r\n2\r00:00:03,000 --> 00:00:04,000\rSecond line\r",
  );
  const p = payload([
    "--subs-file",
    srt,
    "--around",
    "2.5",
    "--window",
    "0",
    "--query",
    "STRASSE",
  ]);
  assert.equal(p.around.length, 1);
  assert.equal(p.matches[0].text, "Hello Straße");
  assert.equal(p.cue_count, 2);
});
test("rolling equal, growing and shrinking cues retain the earliest start and latest end", () => {
  const subs = file(
    "rolling.vtt",
    "0:00 --> 0:01\nhello\n\n0:01 --> 0:02\nhello world\n\n0:02 --> 0:03\nhello\n\n0:03 --> 0:04\nhello world\n\n0:04 --> 0:05\nother\n",
  );
  const p = payload(["--subs-file", subs, "--around", "0", "--window", "10"]);
  assert.equal(p.cue_count, 2);
  assert.deepEqual(p.around[0], { start: 0, end: 4, text: "hello world" });
});
test("replacement decoding, NUL and tag cleanup preserve caption content", () => {
  const subs = file(
    "binary.vtt",
    Buffer.concat([
      Buffer.from("0:00 --> 0:01\n<tag>foo\u0000bar</tag> &amp; &nbsp;"),
      Buffer.from([0xff]),
      Buffer.from("\n"),
    ]),
  );
  const p = payload(["--subs-file", subs, "--query", "BAR"]);
  assert.equal(p.matches[0].text, "foo\u0000bar &amp;  �");
});
test("text rendering covers metadata, timestamps, quoted search, empty results and bounded samples", () => {
  for (const args of [
    [],
    ["--around", "17m31s"],
    ["--around", "214", "--query", "GGUF"],
    ["--query", "doesn't match"],
    ["--query", "\u0085"],
    ["--query", '"both\'"'],
    ["--query", ""],
  ])
    assert.equal(
      check(youtube, ["--info-file", info, "--subs-file", vtt, ...args]).status,
      0,
    );
  const p = check(youtube, ["--info-file", info]);
  assert(p.stdout.endsWith("Captions: none\n"));
  const many = file(
    "many.vtt",
    Array.from(
      { length: 15 },
      (_, i) =>
        `0:${String(i).padStart(2, "0")} --> 0:${String(i + 1).padStart(2, "0")}\nrow ${String(i).padStart(2, "0")}\n`,
    ).join("\n"),
  );
  const r = check(youtube, ["--subs-file", many]);
  assert.equal(r.stdout.match(/^\[/gm)?.length, 12);
});
test("empty metadata, uploader fallback, chapters and negative windows keep the schema", () => {
  const path = file(
    "fallback.json",
    JSON.stringify({
      uploader: "Uploader",
      duration: 3661,
      chapters: [{ title: "Start" }, { start_time: "2.5", title: "" }],
    }),
  );
  const p = payload([
    "--info-file",
    path,
    "--subs-file",
    vtt,
    "--window",
    "-1",
    "--around",
    "214",
  ]);
  assert.equal(p.channel, "Uploader");
  assert.equal(p.duration_string, "1:01:01");
  assert.equal(p.chapters[0].start, 0);
  assert.equal(p.id, null);
  assert.equal(p.window, -1);
  assert.deepEqual(payload(["--subs-file", file("empty.vtt", "")]).around, []);
});
test("invalid timestamps, malformed cues/JSON, missing files and usage fail visibly", () => {
  for (const args of [
    [],
    ["--info-file", "/missing/info"],
    ["--subs-file", file("bad.vtt", "nonsense --> 0:01\ntext\n")],
    ["--info-file", file("bad.json", "{")],
    ["--subs-file", vtt, "--around", "invalid"],
    ["--subs-file", vtt, "--around", " "],
    ["--subs-file", vtt, "--window", "bad"],
  ]) {
    const r = check(youtube, args);
    assert.notEqual(r.status, 0);
  }
  assert.equal(run(youtube, ["--help"]).status, 0);
});
function downloader(name = "yt-dlp") {
  const dir = join(root, "bin-" + serial++);
  fs.mkdirSync(dir);
  const log = file("download-log", "");
  fs.writeFileSync(
    join(dir, name),
    `#!${process.execPath}\nconst fs=require('node:fs'),p=require('node:path');const args=process.argv.slice(2);fs.writeFileSync(process.env.DOWNLOAD_LOG,JSON.stringify(args));if(process.env.DOWNLOAD_MODE==='fail')process.exit(17);const out=args[args.indexOf('-o')+1],dir=p.dirname(out);fs.mkdirSync(dir,{recursive:true});if(process.env.DOWNLOAD_MODE!=='no-info')fs.writeFileSync(p.join(dir,'video.info.json'),JSON.stringify({title:'stub title',uploader:'stub uploader',duration:61}));if(process.env.DOWNLOAD_MODE!=='no-subs'){fs.writeFileSync(p.join(dir,'a.auto.vtt'),'0:00 --> 0:01\\nauto\\n');fs.writeFileSync(p.join(dir,'z.srt'),'0:00 --> 0:01\\nmanual\\n');}\n`,
    { mode: 0o755 },
  );
  return { PATH: dir, DOWNLOAD_LOG: log };
}
test("host yt-dlp uses only metadata/subtitle flags, selects manual captions and cleans temporary data", () => {
  const env = downloader();
  const p = payload(["https://youtu.be/qILTuXLxfBM", "--query", "manual"], env);
  assert.equal(p.matches[0].text, "manual");
  const args = JSON.parse(fs.readFileSync(env.DOWNLOAD_LOG, "utf8"));
  assert.deepEqual(args.slice(0, -2), [
    "--skip-download",
    "--no-playlist",
    "--no-warnings",
    "--write-info-json",
    "--write-subs",
    "--write-auto-subs",
    "--sub-langs",
    "en.*,en",
    "--sub-format",
    "vtt/best",
    "-o",
  ]);
  assert(!args.includes("-x") && !args.includes("-f"));
  assert.equal(fs.existsSync(dirname(args.at(-2))), false);
});
test("Nix fallback keeps existing work directories, handles missing captions and propagates fetch failure", () => {
  const env = downloader("nix"),
    workdir = join(root, "work");
  fs.mkdirSync(workdir);
  let p = payload(
    ["--url", "https://youtu.be/qILTuXLxfBM", "--workdir", workdir],
    { ...env, DOWNLOAD_MODE: "no-subs" },
  );
  assert.equal(p.cue_count, 0);
  assert(fs.existsSync(join(workdir, "video.info.json")));
  const args = JSON.parse(fs.readFileSync(env.DOWNLOAD_LOG, "utf8"));
  assert.deepEqual(args.slice(0, 4), [
    "shell",
    "nixpkgs#yt-dlp",
    "-c",
    "yt-dlp",
  ]);
  for (const mode of ["fail", "no-info"])
    assert.equal(
      check(youtube, ["https://youtu.be/qILTuXLxfBM"], {
        ...env,
        DOWNLOAD_MODE: mode,
      }).status,
      1,
    );
  const missing = check(youtube, ["https://youtu.be/qILTuXLxfBM"], {
    PATH: root,
  });
  assert.equal(missing.status, 1);
});
