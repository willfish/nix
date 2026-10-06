import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import { createInterface } from "node:readline";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { test, afterEach } from "node:test";
import { fileURLToPath } from "node:url";
const source = dirname(fileURLToPath(import.meta.url));
const bin = resolve(process.env.MEMSCOPE_BIN || join(source, "memscope"));
const helper = resolve(
  process.env.MEMSCOPE_FIXTURE || join(source, "test_support"),
);
const roots: string[] = [];
afterEach(() => {
  for (const p of roots.splice(0))
    fs.rmSync(p, { recursive: true, force: true });
});
function directory() {
  const root = fs.mkdtempSync(join(tmpdir(), "memscope-"));
  roots.push(root);
  return root;
}
function mapping({
  inode = 1,
  path = "/models/weights.gguf",
  pss = 1048576,
  flags = "rd mr mw me",
  anon = 0,
}: any = {}) {
  return `10000000-90000000 r--p 00000000 08:01 ${inode} ${path}\nSize: 2097152 kB\nRss: ${pss} kB\nPss: ${pss} kB\nAnonymous: ${anon} kB\nShared_Hugetlb: 0 kB\nPrivate_Hugetlb: 0 kB\nVmFlags: ${flags}\n`;
}
function fixture() {
  const root = directory();
  fs.writeFileSync(
    join(root, "meminfo"),
    "MemTotal: 33554432 kB\nMemAvailable: 12582912 kB\nAnonPages: 16777216 kB\n",
  );
  function proc(pid: number, name: string, smaps: string) {
    const p = join(root, String(pid));
    fs.mkdirSync(p, { recursive: true });
    fs.writeFileSync(join(p, "comm"), name + "\n");
    fs.writeFileSync(join(p, "smaps"), smaps);
    return p;
  }
  proc(10, "llama-server", mapping());
  proc(
    11,
    "browser",
    mapping({ inode: 2, path: "/cache/media-cache.db", pss: 524288 }),
  );
  proc(
    12,
    "audio-server",
    mapping({ inode: 3, path: "/lib/libcuda.so.1", pss: 262144 }),
  );
  proc(13, "other", mapping({ inode: 4, path: "/lib/other.so", pss: 262144 }));
  return { root, proc };
}
function run(root: string, args: string[] = [], env: NodeJS.ProcessEnv = {}) {
  return spawnSync(bin, ["--proc-root", root, ...args], {
    encoding: "utf8",
    env: { ...process.env, NO_COLOR: "", TERM: "xterm-256color", ...env },
    timeout: 10000,
  });
}
function output(
  root: string,
  args: string[] = [],
  env: NodeJS.ProcessEnv = {},
) {
  const r = run(root, args, env);
  assert.equal(r.status, 0, r.stderr);
  return r.stdout;
}
async function until<T>(promise: Promise<T>, ms = 5000): Promise<T> {
  let timer: NodeJS.Timeout | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timer = setTimeout(
          () => reject(new Error("fixture process timed out")),
          ms,
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}
const split = (s: string) => s.split(/\r?\n/).filter(Boolean);
const plain = (s: string) => s.replace(/\x1b\[[0-9;]*m/g, "");
function width(s: string, n: number) {
  assert(
    split(s).every((line) => [...line].length <= n),
    s,
  );
}
test("headline and denominators", () => {
  const f = fixture(),
    out = output(f.root, ["--limit", "3"]);
  assert(out.includes("20.0 GiB / 32.0 GiB used   62.5%"));
  assert(out.includes("12.0 GiB available"));
  assert(out.includes("2.0 GiB PSS | 6.25% of total RAM"));
  assert.match(out, /weights.gguf.*50.0%/);
  assert.match(out, /Other observed mappings.*12.5%/);
  const one = output(f.root, ["--limit", "1"]);
  assert.match(one, /weights.gguf.*50.0%/);
  assert.match(one, /Other observed mappings.*50.0%/);
  assert(!out.includes("\x1b"));
});
test("identity aliases and deleted files", () => {
  const f = fixture();
  f.proc(14, "second", mapping({ inode: 1, path: "/alias/model (deleted)" }));
  const out = output(f.root);
  assert(out.includes("3.0 GiB PSS"));
  assert.match(out, /66.7%/);
  assert(out.includes("[deleted]"));
  assert(out.includes("second"));
  f.proc(
    15,
    "different",
    mapping({ inode: 99, path: "/another/weights.gguf", pss: 200000 }),
  );
  assert(output(f.root).includes("identity"));
});
test("private copies are not subtracted", () => {
  const f = fixture();
  f.proc(10, "llama-server", mapping({ anon: 524288 }));
  assert(output(f.root).includes("2.0 GiB PSS"));
});
test("special mappings are excluded", () => {
  const f = fixture();
  f.proc(
    20,
    "device",
    mapping({ inode: 200, path: "/dev/example", flags: "rd io pf" }),
  );
  const out = output(f.root);
  assert(out.includes("2.0 GiB PSS"));
  assert(out.includes("1 special mappings excluded"));
});
test("shared memory is identified", () => {
  const f = fixture();
  f.proc(10, "shared", mapping({ path: "/memfd:example (deleted)" }));
  assert(output(f.root).includes("[shmem]"));
});
test("future fields and empty or vanished processes", () => {
  const f = fixture();
  const fields =
    "AnonHugePages: 0 kB\nFilePmdMapped: 0 kB\nFuture_Field: 123 kB\n";
  f.proc(
    10,
    "llama-server",
    mapping().replace("VmFlags:", fields + "VmFlags:"),
  );
  f.proc(90, "kernel-thread", "");
  const vanished = f.proc(91, "exited", "");
  fs.unlinkSync(join(vanished, "smaps"));
  const out = output(f.root);
  assert(out.includes("2.0 GiB PSS"));
  assert(out.includes("4 read"));
  assert(out.includes("1 vanished"));
  assert(!out.includes("invalid process"));
});
test("PSS overflow discards the whole process", () => {
  const f = fixture();
  f.proc(
    10,
    "bad",
    mapping({ pss: 18446744073709551615n }) + mapping({ inode: 20, pss: 1 }),
  );
  const out = output(f.root);
  assert(out.includes("1 invalid"));
  assert(out.includes("1.0 GiB PSS"));
  assert(!out.includes("weights.gguf"));
});
test("clipped names retain identity", () => {
  const f = fixture(),
    suffix = "same-suffix-012345678901234567890";
  f.proc(10, "first", mapping({ inode: 1, path: "/models/first-" + suffix }));
  f.proc(
    11,
    "second",
    mapping({ inode: 2, path: "/models/second-" + suffix, pss: 524288 }),
  );
  const out = output(f.root, ["--width", "80"]);
  assert(out.includes("identity 8:1:1"));
  assert(out.includes("identity 8:1:2"));
});
test("embedded comm newlines are escaped", () => {
  const f = fixture();
  f.proc(10, "worker\nA", mapping());
  assert(output(f.root).includes("worker\\x0aA"));
});
test("full paths remain available", () => {
  const f = fixture(),
    name = "/a/very/long/path/" + "component/".repeat(10) + "weights.gguf";
  f.proc(10, "llama-server", mapping({ path: name }));
  assert(
    split(output(f.root, ["--full-paths"]))
      .join("")
      .includes(name),
  );
});
test("missing availability never falls back to free memory", () => {
  const f = fixture();
  fs.writeFileSync(
    join(f.root, "meminfo"),
    "MemTotal: 33554432 kB\nMemFree: 123 kB\n",
  );
  assert(output(f.root).includes("used/available unavailable"));
});
test("invalid and overflowing total memory fail", () => {
  const f = fixture();
  for (const text of [
    "MemTotal: 0 kB\n",
    "MemTotal: 18446744073709551616 kB\n",
    "MemTotal: -1 kB\n",
    "MemTotal: 32 MB\n",
  ]) {
    fs.writeFileSync(join(f.root, "meminfo"), text);
    assert.notEqual(run(f.root).status, 0);
  }
});
test("malformed process data is transactional", () => {
  const f = fixture();
  f.proc(
    10,
    "bad",
    mapping() + mapping({ inode: 19, pss: 9999 }).split("VmFlags:")[0],
  );
  const out = output(f.root);
  assert(out.includes("1 invalid"));
  assert(out.includes("1.0 GiB PSS"));
  assert(!out.includes("weights.gguf"));
});
test("empty proc trees report unavailable mapping PSS", () => {
  const f = fixture();
  for (const p of fs.readdirSync(f.root))
    if (/^\d+$/.test(p)) fs.rmSync(join(f.root, p), { recursive: true });
  assert(output(f.root).includes("Mapping PSS unavailable"));
});
test("attachment ellipsis and expansion", () => {
  const f = fixture();
  for (let pid = 20; pid < 28; pid++)
    f.proc(pid, `process-${pid}-with-a-long-name`, mapping({ pss: 1024 }));
  const compact = output(f.root, ["--width", "40"]);
  assert(compact.includes("..."));
  width(compact, 40);
  assert(
    output(f.root, ["--width", "40", "--full-paths"]).includes(
      "process-20-with-a-long-name",
    ),
  );
});
test("zero and unavailable remain distinct", () => {
  const f = fixture();
  for (const p of fs.readdirSync(f.root))
    if (/^\d+$/.test(p))
      fs.writeFileSync(join(f.root, p, "smaps"), mapping({ pss: 0 }));
  const out = output(f.root);
  assert(out.includes("No resident PSS"));
  assert(!out.toLowerCase().includes("nan"));
  for (const p of fs.readdirSync(f.root))
    if (/^\d+$/.test(p)) fs.writeFileSync(join(f.root, p, "smaps"), "bad\n");
  assert(output(f.root).includes("Mapping PSS unavailable"));
});
test(
  "permission failures remain partial",
  { skip: process.getuid!() === 0 },
  () => {
    const f = fixture(),
      path = join(f.root, "10/smaps");
    fs.chmodSync(path, 0);
    try {
      const out = output(f.root);
      assert(out.includes("1 denied"));
      assert(out.includes("PARTIAL"));
      assert(!out.includes("weights.gguf"));
    } finally {
      fs.chmodSync(path, 0o600);
    }
  },
);
test("terminal injection and width limits", () => {
  const f = fixture();
  f.proc(10, "bad\x1b[2Jname", mapping({ path: "/evil/\x1b[31mweights.gguf" }));
  for (const n of [8, 20, 40, 60, 80, 120]) {
    const out = output(f.root, ["--width", String(n)]);
    assert(!out.includes("\x1b"));
    width(out, n);
  }
  assert(output(f.root).includes("\\x1b"));
});
test("color contract and CLI options", () => {
  const f = fixture(),
    styled = output(f.root, ["--color", "always"]);
  for (const sgr of ["34", "0;35", "36", "32", "1", "1;39"])
    assert(styled.includes(`\x1b[${sgr}m`));
  assert.equal(
    plain(styled).replace(/\d+ ms sample/g, "TIME"),
    output(f.root, ["--color", "never"]).replace(/\d+ ms sample/g, "TIME"),
  );
  assert(
    !output(f.root, ["--color", "always"], { NO_COLOR: "1" }).includes("\x1b"),
  );
  assert(!output(f.root, [], { TERM: "dumb" }).includes("\x1b"));
  for (const args of [
    ["--limit", "0"],
    ["--limit", "-1"],
    ["--width", "999"],
    ["--color", "invalid"],
    ["unexpected"],
  ])
    assert.equal(run(f.root, args).status, 2);
  assert(output(f.root, ["--version"]).includes("memscope 0.1.0"));
  assert(output(f.root, ["--help"]).includes("MemTotal - MemAvailable"));
});
test("PTY widths and Unicode bars", () => {
  const f = fixture();
  for (const n of [40, 60, 80, 120]) {
    const r = spawnSync(helper, ["pty", String(n), bin, f.root], {
      encoding: "utf8",
      timeout: 10000,
      env: {
        ...process.env,
        TERM: "xterm-256color",
        LC_ALL: "C.UTF-8",
        NO_COLOR: "",
      },
    });
    assert.equal(r.status, 0, r.stderr);
    assert(r.stdout.includes("\x1b[34m"));
    assert(r.stdout.includes("█"));
    width(plain(r.stdout), n);
  }
});
async function mapped(mode: string, path: string) {
  const child = spawn(helper, [mode, path], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  const ended = once(child, "close");
  let stderr = "";
  child.stderr.on("data", (b) => (stderr += b));
  const reader = createInterface({ input: child.stdout });
  const iterator = reader[Symbol.asyncIterator]();
  const first = await until(iterator.next());
  assert.equal(first.value, "ready", stderr);
  return {
    child,
    async command(text: string) {
      child.stdin.write(text + "\n");
      const line = await until(iterator.next());
      assert(!line.done, stderr);
      return line.value;
    },
    async stop() {
      child.stdin.end("quit\n");
      const [code] = await until(ended);
      assert.equal(code, 0, stderr);
      reader.close();
    },
  };
}
function liveRoot(pids: number[]) {
  const root = directory();
  fs.writeFileSync(join(root, "meminfo"), fs.readFileSync("/proc/meminfo"));
  for (const pid of pids)
    fs.symlinkSync("/proc/" + pid, join(root, String(pid)));
  return root;
}
test("a real process can vanish after PID enumeration", async () => {
  const root = directory();
  fs.writeFileSync(join(root, "meminfo"), "MemTotal: 33554432 kB\n");
  fs.mkdirSync(join(root, "1"));
  fs.writeFileSync(join(root, "1/comm"), "barrier\n");
  const fifo = join(root, "1/smaps");
  assert.equal(spawnSync("mkfifo", [fifo]).status, 0);
  const child = spawn(process.execPath, ["-e", "setTimeout(()=>{},30000)"]);
  const childEnd = once(child, "close");
  fs.symlinkSync("/proc/" + child.pid, join(root, String(child.pid)));
  const collector = spawn(bin, ["--proc-root", root], {
    env: { ...process.env, NO_COLOR: "1" },
  });
  const collectorEnd = once(collector, "close");
  let out = "",
    err = "";
  collector.stdout.on("data", (b) => (out += b));
  collector.stderr.on("data", (b) => (err += b));
  let writer: number | undefined;
  try {
    const deadline = Date.now() + 5000;
    while (writer === undefined) {
      try {
        writer = fs.openSync(
          fifo,
          fs.constants.O_WRONLY | fs.constants.O_NONBLOCK,
        );
      } catch (error: any) {
        if (error.code !== "ENXIO" || Date.now() > deadline) throw error;
        await new Promise((r) => setTimeout(r, 10));
      }
    }
    child.kill("SIGTERM");
    await until(childEnd);
    fs.writeSync(writer, mapping());
    fs.closeSync(writer);
    writer = undefined;
    const [code] = await until(collectorEnd);
    assert.equal(code, 0, err);
    assert(out.includes("1 read"));
    assert(out.includes("1 vanished"));
    assert(out.includes("PARTIAL"));
  } finally {
    if (writer !== undefined) fs.closeSync(writer);
    if (child.exitCode === null && child.signalCode === null)
      child.kill("SIGKILL");
    if (collector.exitCode === null && collector.signalCode === null)
      collector.kill("SIGKILL");
    await Promise.all([childEnd, collectorEnd]);
  }
});
test("live shared/private copies and deleted mapping accounting", async () => {
  const path = join(directory(), "memscope-live-fixture"),
    m = await mapped("cow", path);
  try {
    const root = liveRoot([m.child.pid!]);
    const before = Number(await m.command("pss"));
    assert(before >= 8000 && before <= 8300, String(before));
    assert.equal(await m.command("copy"), "copied");
    const after = Number(await m.command("pss"));
    assert(after >= 16000 && after <= 16600, String(after));
    assert.equal(await m.command("unlink"), "unlinked");
    const out = output(root, ["--limit", "1000"]);
    assert(out.includes("[deleted]"));
    assert(
      split(out)
        .find((s) => s.includes("memscope-live-fixture"))
        ?.includes("16 MiB"),
    );
  } finally {
    await m.stop();
  }
});
test("shared files across processes are not double-counted", async () => {
  const path = join(directory(), "shared-live-fixture"),
    a = await mapped("shared", path),
    b = await mapped("shared", path);
  try {
    const root = liveRoot([a.child.pid!, b.child.pid!]);
    const out = output(root, ["--limit", "1000"]);
    assert(
      split(out)
        .find((s) => s.includes("shared-live-fixture"))
        ?.includes("8 MiB"),
    );
    assert(out.includes("2 read"));
  } finally {
    await Promise.all([a.stop(), b.stop()]);
  }
});
test("live memfd is labelled as shared memory", async () => {
  const m = await mapped("memfd", "unused");
  try {
    const root = liveRoot([m.child.pid!]);
    const out = output(root, ["--limit", "1000"]);
    assert(out.includes("memfd:memscope-memfd"));
    assert(out.includes("[shmem]"));
  } finally {
    await m.stop();
  }
});
