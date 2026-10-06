import assert from "node:assert/strict";
import { spawn, type ChildProcess } from "node:child_process";
import {
  chmodSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import test from "node:test";
const fixture = process.env.NM_AGENT_FIXTURE!,
  peer = process.env.NM_AGENT_PEER!,
  production = process.env.NM_AGENT_BIN;
assert.ok(fixture && peer, "Set NM_AGENT_FIXTURE and NM_AGENT_PEER");
class Process {
  child: ChildProcess;
  lines: string[] = [];
  stderr = "";
  stdout = "";
  code: number | null | undefined;
  signal: string | null = null;
  constructor(file: string, args: string[], env: any) {
    this.child = spawn(file, args, { env, stdio: ["pipe", "pipe", "pipe"] });
    let buffer = "";
    this.child.stdout!.on("data", (b) => {
      this.stdout += b;
      buffer += b;
      let i;
      while ((i = buffer.indexOf("\n")) >= 0) {
        this.lines.push(buffer.slice(0, i));
        buffer = buffer.slice(i + 1);
      }
    });
    this.child.stderr!.on("data", (b) => (this.stderr += b));
    this.child.on("exit", (c, s) => {
      this.code = c;
      this.signal = s;
    });
    this.child.on("error", (e) => {
      this.stderr += e;
      this.code = 125;
    });
  }
  async line(prefix: string, timeout = 7000) {
    for (let i = 0; i < timeout / 10; i++) {
      const at = this.lines.findIndex((s) => s.startsWith(prefix));
      if (at >= 0) return this.lines.splice(at, 1)[0];
      if (this.code !== undefined)
        throw Error("child exited " + this.code + " " + this.stderr);
      await sleep(10);
    }
    throw Error(
      "waiting for " +
        prefix +
        "; stderr=" +
        this.stderr +
        "; lines=" +
        this.lines.join(","),
    );
  }
  send(command: string) {
    this.child.stdin!.write(command + "\n");
  }
  async exit(timeout = 7000) {
    for (let i = 0; i < timeout / 10; i++) {
      if (this.code !== undefined) return this.code;
      await sleep(10);
    }
    throw Error("exit timeout " + this.stderr);
  }
  async kill() {
    if (this.code === undefined) {
      this.child.kill("SIGKILL");
      await this.exit();
    }
  }
}
class Session {
  legacyMode: boolean;
  constructor(legacyMode = false) {
    this.legacyMode = legacyMode;
  }
  root = mkdtempSync(join(tmpdir(), "nm-agent-bus-"));
  children: Process[] = [];
  env: any;
  manager!: Process;
  service!: Process;
  name = "";
  add(file: string, args: string[], env: any) {
    const p = new Process(file, args, env);
    this.children.push(p);
    return p;
  }
  async init(own = true) {
    const bus = this.add(
      "dbus-daemon",
      ["--session", "--nofork", "--print-address=1"],
      { ...process.env },
    );
    const address = await bus.line("unix:");
    this.env = {
      ...process.env,
      DBUS_SESSION_BUS_ADDRESS: address,
      DBUS_SYSTEM_BUS_ADDRESS: address,
      LIBNM_USE_SESSION_BUS: "1",
      G_DEBUG: "fatal-criticals",
      NM_TEST_ROOT: this.root,
      NM_AGENT_NMCLI: "/never-use-runtime-command-overrides",
    };
    delete this.env.LD_PRELOAD;
    this.manager = this.add(peer, [], this.env);
    await this.manager.line("READY");
    if (own) {
      this.manager.send("own");
      await this.manager.line("OWNED");
    }
    const script =
      '#!/usr/bin/env bash\nprintf "%s\\n" "$@" > "$NM_TEST_ROOT/args"\nprintf "%s\\n" "$$" > "$NM_TEST_ROOT/pid"\nmode=$(cat "$NM_TEST_ROOT/mode")\ncase "$mode" in\nfail) cat "$NM_TEST_ROOT/value"; cat "$NM_TEST_ROOT/value" >&2; exit 9;;\nwait) exec sleep 30;;\n*) cat "$NM_TEST_ROOT/value";;\nesac\n';
    writeFileSync(join(this.root, "nmcli"), script);
    chmodSync(join(this.root, "nmcli"), 0o700);
    this.value("fixture-only-passphrase\n");
    return this;
  }
  value(value: string | Buffer, mode = "ok") {
    writeFileSync(join(this.root, "value"), value);
    writeFileSync(join(this.root, "mode"), mode);
  }
  start(system = false) {
    const env = { ...this.env };
    if (system) delete env.LIBNM_USE_SESSION_BUS;
    if (this.legacyMode) {
      assert.ok(
        process.env.NM_AGENT_LEGACY &&
          process.env.NM_AGENT_PYTHON &&
          process.env.NM_AGENT_ROUTE,
      );
      env.LD_PRELOAD = process.env.NM_AGENT_ROUTE;
      env.NM_FIXTURE_EXEC = join(this.root, "nmcli");
      this.service = this.add(
        process.env.NM_AGENT_PYTHON!,
        [process.env.NM_AGENT_LEGACY!],
        env,
      );
    } else if (production) {
      assert.ok(
        process.env.NM_AGENT_ROUTE,
        "packaged command needs routing interposer",
      );
      env.LD_PRELOAD = process.env.NM_AGENT_ROUTE;
      env.NM_FIXTURE_EXEC = join(this.root, "nmcli");
      this.service = this.add(production, [], env);
    } else
      this.service = this.add(fixture, ["run", join(this.root, "nmcli")], env);
  }
  async ready() {
    const line = await this.manager.line("REGISTER\t");
    const fields = line.split("\t");
    this.name = fields[1];
    assert.equal(fields[2], "org.dotfiles.nm-auto-secret-agent");
    assert.equal(fields[3], "0");
    for (
      let i = 0;
      i < 500 && !this.service.stderr.includes("registered=True");
      i++
    )
      await sleep(10);
    assert.match(this.service.stderr, /registered=True/);
  }
  args(
    setting = "802-11-wireless-security",
    flags = 0,
    id = "Fixture Wi-Fi",
    uuid = "13572468-1234-4321-abcd-123456789012",
    path = "/org/freedesktop/NetworkManager/Settings/1",
  ) {
    const props = [
      ...(id ? ["'id': <" + quote(id) + ">"] : []),
      ...(uuid ? ["'uuid': <" + quote(uuid) + ">"] : []),
      "'type': <'802-11-wireless'>",
    ].join(", ");
    return (
      "(@a{sa{sv}} {'connection': {" +
      props +
      "}}, objectpath " +
      quote(path) +
      ", " +
      quote(setting) +
      ", @as [], uint32 " +
      flags +
      ")"
    );
  }
  async call(method: string, args: string) {
    this.manager.send("call\t" + method + "\t" + args);
    for (let i = 0; i < 700; i++) {
      const at = this.manager.lines.findIndex(
        (s) => s.startsWith("REPLY\t") || s.startsWith("ERROR\t"),
      );
      if (at >= 0) return this.manager.lines.splice(at, 1)[0];
      if (this.service?.code !== undefined)
        throw Error(
          "service exited " + this.service.code + " " + this.service.stderr,
        );
      await sleep(10);
    }
    throw Error("call timed out " + this.service.stderr);
  }
  async stop(signal: "SIGTERM" | "SIGINT" = "SIGTERM", code = 0) {
    this.service.child.kill(signal);
    assert.equal(await this.service.exit(), code, this.service.stderr);
    assert.match(this.service.stderr, /signal (?:15|2); shutting down/);
    assert.doesNotMatch(
      this.service.stderr,
      /fixture-only-passphrase|GLib-CRITICAL|panic/,
    );
  }
  async close() {
    for (const p of this.children.toReversed()) await p.kill();
    rmSync(this.root, { recursive: true, force: true });
  }
}
function quote(s: string) {
  return (
    "'" +
    s
      .replaceAll("\\", "\\\\")
      .replaceAll("'", "\\'")
      .replaceAll("\n", "\\n")
      .replaceAll("\r", "\\r")
      .replaceAll("\t", "\\t") +
    "'"
  );
}
async function session(fn: (s: Session) => Promise<void>, own = true) {
  const s = new Session();
  try {
    await s.init(own);
    await fn(s);
  } finally {
    await s.close();
  }
}

test("registers with the existing identifier and capabilities; returns libnm Wi-Fi serialization for normal and REQUEST_NEW", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    for (const flags of [0, 1, 2, 3, 4, 0x80000000]) {
      const reply = await s.call("GetSecrets", s.args(undefined, flags));
      assert.match(reply, /^REPLY\t/);
      assert.match(reply, /'802-11-wireless-security'/);
      assert.match(reply, /'psk': <'fixture-only-passphrase'>/);
      const args = readFileSync(join(s.root, "args"), "utf8")
        .trimEnd()
        .split("\n");
      assert.deepEqual(args, [
        "-s",
        "-g",
        "802-11-wireless-security.psk",
        "connection",
        "show",
        "13572468-1234-4321-abcd-123456789012",
      ]);
    }
    await s.stop();
    await s.manager.line("UNREGISTER");
  }));
test("ID fallback, Unicode and metacharacters stay literal without a shell", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    const id = "Fixture 尾;$(touch NEVER) 'quote'";
    const reply = await s.call("GetSecrets", s.args(undefined, 0, id, ""));
    assert.match(reply, /^REPLY/);
    assert.equal(readFileSync(join(s.root, "args"), "utf8").split("\n")[5], id);
    assert.equal(existsSync(join(s.root, "NEVER")), false);
    await s.stop("SIGINT");
  }));
test("missing keys and unsupported settings return NoSecrets without spawning nmcli", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    let result = await s.call("GetSecrets", s.args("vpn", 0, "", ""));
    assert.match(result, /NoSecrets/);
    assert.match(result, /connection has no uuid\/id/);
    result = await s.call("GetSecrets", s.args("vpn"));
    assert.match(result, /NoSecrets/);
    assert.match(result, /unsupported setting for auto agent: vpn/);
    assert.equal(existsSync(join(s.root, "args")), false);
    await s.stop();
  }));
test("empty, invalid UTF-8, embedded NUL and failed child output fail without leaking credentials", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    for (const [data, mode, message] of [
      [Buffer.from("\r\n\r"), "ok", "empty psk"],
      [Buffer.from([83, 69, 67, 82, 69, 84, 255]), "ok", "invalid UTF-8"],
      [Buffer.from("SECRET\0tail"), "ok", "invalid string"],
      [Buffer.from("SECRET"), "fail", "status 9"],
    ] as [Buffer, string, string][]) {
      s.value(data, mode);
      const result = await s.call("GetSecrets", s.args());
      assert.match(result, /NoSecrets/);
      assert.ok(result.includes(message), result);
      assert.doesNotMatch(result, /SECRET/);
      assert.doesNotMatch(s.service.stderr, /SECRET/);
    }
    await s.stop();
  }));
test("universal newline conversion strips only trailing LF and retains spaces", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    s.value(" a\r\nb\rc \r\n\n");
    const result = await s.call("GetSecrets", s.args());
    assert.match(result, /'psk': <' a\\nb\\nc '>/);
    await s.stop();
  }));
test("SaveSecrets and DeleteSecrets are successful no-ops; absent cancellation remains a libnm error", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    const args =
      "(@a{sa{sv}} {'connection': {'id': <'Fixture'>, 'type': <'802-11-wireless'>}}, objectpath '/org/freedesktop/NetworkManager/Settings/1')";
    for (const method of ["SaveSecrets", "DeleteSecrets"])
      assert.match(await s.call(method, args), /^REPLY\t\(\)/);
    const cancel = await s.call(
      "CancelGetSecrets",
      "(objectpath '/org/freedesktop/NetworkManager/Settings/1', '802-11-wireless-security')",
    );
    assert.match(cancel, /No secrets request in progress/);
    assert.equal(existsSync(join(s.root, "args")), false);
    await s.stop();
  }));
test("invalid connection paths retain libnm validation before application callbacks", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    const result = await s.call(
      "GetSecrets",
      s.args(undefined, 0, "Fixture", "", "/"),
    );
    assert.match(result, /InvalidConnection/);
    assert.equal(existsSync(join(s.root, "args")), false);
    await s.stop();
  }));
test("manager disappearance and return re-register without losing successful exit state", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    s.manager.send("release");
    await s.manager.line("RELEASED");
    await s.manager.line("UNREGISTER");
    await sleep(100);
    s.manager.send("own");
    await s.manager.line("OWNED");
    await s.ready();
    assert.match(await s.call("GetSecrets", s.args()), /^REPLY/);
    await s.stop();
  }));
test("registration refusal exits unsuccessfully without secret lookup", () =>
  session(async (s) => {
    s.manager.send("deny");
    await s.manager.line("DENY");
    s.start();
    await s.manager.line("REGISTER\t");
    assert.equal(await s.service.exit(), 1, s.service.stderr);
    assert.match(s.service.stderr, /register failed: registration failed/);
    assert.equal(existsSync(join(s.root, "args")), false);
  }));
test("unavailable disposable bus fails initialization without a secret lookup", () =>
  session(async (s) => {
    await s.children[0].kill();
    s.start();
    assert.equal(await s.service.exit(), 1, s.service.stderr);
    assert.match(s.service.stderr, /initialization failed:/);
    assert.equal(existsSync(join(s.root, "args")), false);
  }));
test("missing manager fails initial registration and does not persist a dormant process", () =>
  session(async (s) => {
    s.start();
    assert.equal(await s.service.exit(), 1, s.service.stderr);
    assert.match(s.service.stderr, /register failed:/);
    assert.equal(existsSync(join(s.root, "args")), false);
  }, false));
test("system-bus manager owned by nonroot is not trusted, and requests are rejected before nmcli", () =>
  session(async (s) => {
    s.manager.send("names");
    const before = await s.manager.line("REPLY\t");
    s.start(true);
    let names = "";
    for (let i = 0; i < 100; i++) {
      s.manager.send("names");
      names = await s.manager.line("REPLY\t");
      if (names !== before) break;
      await sleep(10);
    }
    const oldNames = new Set(before.match(/:\d+\.\d+/g));
    const added = (names.match(/:\d+\.\d+/g) || []).filter(
      (n) => !oldNames.has(n),
    );
    assert.equal(added.length, 1);
    s.manager.send("target\t" + added[0]);
    await s.manager.line("TARGET");
    await sleep(100);
    assert.equal(
      s.manager.lines.some((x) => x.startsWith("REGISTER")),
      false,
    );
    const result = await s.call("GetSecrets", s.args());
    assert.match(result, /PermissionDenied/);
    assert.match(result, /non authenticated peer rejected/);
    assert.equal(existsSync(join(s.root, "args")), false);
    await s.stop("SIGTERM", 1);
  }));
test("SIGTERM during a blocked lookup terminates and reaps the child without publishing its secret", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    s.value("fixture-only-passphrase", "wait");
    s.manager.send("call\tGetSecrets\t" + s.args());
    for (let i = 0; i < 400 && !existsSync(join(s.root, "pid")); i++)
      await sleep(10);
    const pid = Number(readFileSync(join(s.root, "pid"), "utf8"));
    await s.stop();
    assert.throws(() => process.kill(pid, 0));
    const reply = await s.manager.line("ERROR\t");
    assert.match(reply, /AgentCanceled/);
    assert.match(s.service.stderr, /CancelGetSecrets path=/);
  }));
test(
  "callback-arity-adapted legacy reference matches payloads and policy errors on a disposable bus",
  { skip: !process.env.NM_AGENT_LEGACY },
  async () => {
    const results: string[][] = [];
    for (const old of [false, true]) {
      const s = new Session(old);
      try {
        await s.init();
        s.start();
        await s.ready();
        const replies = [];
        for (const value of [
          "fixture-only-passphrase\n",
          " x\r\ny \n",
          "尾🙂\n",
        ]) {
          s.value(value);
          const reply = await s.call("GetSecrets", s.args(undefined, 2));
          assert.doesNotMatch(reply, /Timeout was reached/, s.service.stderr);
          replies.push(reply);
        }
        replies.push(await s.call("GetSecrets", s.args("vpn")));
        replies.push(await s.call("GetSecrets", s.args("vpn", 0, "", "")));
        s.value("\n");
        replies.push(await s.call("GetSecrets", s.args()));
        results.push(replies);
        await s.stop();
      } finally {
        await s.close();
      }
    }
    assert.deepEqual(results[0], results[1]);
  },
);
test(
  "host enablement and unchanged reconnect watcher retain the private graphical boundary",
  { skip: !process.env.NM_AGENT_WIRING },
  () => {
    const wiring = JSON.parse(
      readFileSync(process.env.NM_AGENT_WIRING!, "utf8"),
    );
    for (const name of [
      "william@foundation",
      "william@andromeda",
      "william@starfish",
    ]) {
      const c = wiring[name];
      assert.equal(c.private, true);
      assert.deepEqual(c.agent.Unit.After, ["graphical-session.target"]);
      assert.deepEqual(c.agent.Unit.PartOf, ["graphical-session.target"]);
      assert.deepEqual(c.agent.Install.WantedBy, ["graphical-session.target"]);
      assert.equal(c.agent.Service.Type, "simple");
      assert.equal(c.agent.Service.Restart, "on-failure");
      assert.equal(c.agent.Service.RestartSec, 2);
      assert.match(
        c.agent.Service.ExecStart[0],
        /nm-auto-secret-agent-0\.1\.0\/bin\/nm-auto-secret-agent$/,
      );
      assert.doesNotMatch(c.agent.Service.ExecStart[0], /python/);
      assert.deepEqual(c.watcher.Unit.After, [
        "graphical-session.target",
        "nm-auto-secret-agent.service",
      ]);
      assert.equal(c.watcher.Service.RestartSec, 3);
    }
    for (const name of ["william@terminus", "william-darwin"]) {
      assert.equal(wiring[name].agent, null);
      assert.equal(wiring[name].watcher, null);
    }
    if (process.env.NM_AGENT_PUBLIC_WIRING)
      assert.deepEqual(
        JSON.parse(readFileSync(process.env.NM_AGENT_PUBLIC_WIRING, "utf8")),
        { agent: null, watcher: null },
      );
  },
);
test("repeated requests leave no growing open-descriptor set", () =>
  session(async (s) => {
    s.start();
    await s.ready();
    const { readdirSync } = await import("node:fs");
    const count = () =>
      readdirSync("/proc/" + s.service.child.pid + "/fd").length;
    const before = count();
    for (let i = 0; i < 40; i++)
      assert.match(await s.call("GetSecrets", s.args()), /^REPLY/);
    assert.ok(count() <= before + 1);
    await s.stop();
  }));
