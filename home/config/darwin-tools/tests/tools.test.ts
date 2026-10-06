import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { after, test } from "node:test";
import * as fs from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
const root = fs.realpathSync(fs.mkdtempSync(join(tmpdir(), "darwin-tools-")));
after(() => fs.rmSync(root, { recursive: true, force: true }));
const fixture = process.env.DARWIN_FIXTURE!;
const bin = process.env.DARWIN_BIN!;
assert(fixture && bin, "set DARWIN_FIXTURE and DARWIN_BIN");
let count = 0;
function directory() {
  const p = join(root, String(count++));
  fs.mkdirSync(p);
  return p;
}
function invoke(input: any) {
  const file = join(root, `request-${count++}.json`);
  fs.writeFileSync(file, JSON.stringify(input));
  const r = spawnSync(fixture, [file], {
    encoding: "utf8",
    input: input.stdin,
  });
  assert.equal(r.status, 0, r.stderr);
  assert.equal(r.stderr, "");
  const result = JSON.parse(r.stdout);
  if (
    process.env.DARWIN_REFERENCE &&
    !input.record &&
    ["health", "preflight", "diff"].includes(input.op)
  ) {
    const reference = spawnSync(process.env.DARWIN_REFERENCE, [file], {
      encoding: "utf8",
    });
    assert.equal(reference.status, 0, reference.stderr);
    const expected = JSON.parse(reference.stdout);
    if (input.op === "health") {
      const { time: _time, ...snapshot } = result.snapshot;
      assert.deepEqual(snapshot, expected.snapshot);
    } else if (input.op === "preflight") {
      assert.equal(result.error, expected.error);
      if (!result.error) assert.deepEqual(result.errors, expected.errors);
    } else {
      assert.equal(result.ok, expected.ok);
      if (result.ok) assert.equal(result.diff, expected.diff);
    }
  }
  return result;
}
function health(extra: any = {}): any {
  return {
    op: "health",
    config: {
      services: [
        { label: "app", kind: "running" },
        { label: "sops", kind: "oneshot" },
        { label: "ssh", kind: "socket" },
      ],
      readiness: ["wait", "--timeout", "0"],
    },
    responses: {
      "/bin/launchctl print system/app": {
        code: 0,
        out: "state = running\nlast exit code = 75: EX_TEMPFAIL\nEnvironmentVariables = { TOKEN = mock-sensitive-data; }",
      },
      "/bin/launchctl print system/sops": {
        code: 0,
        out: "state = not running\nlast exit code = 0\n",
      },
      "/bin/launchctl print system/ssh": {
        code: 0,
        out: "state = not running\nlast exit code = 1\n",
      },
      "wait --timeout 0": { code: 0, out: "private readiness output" },
      "/usr/bin/vm_stat": {
        code: 0,
        out: "Pages free: 42.\nPages active: 99.\nPages wired down: 10.\nPages occupied by compressor: 11.\nIgnore: private metric\n",
      },
      "/usr/sbin/sysctl -n vm.swapusage": {
        code: 0,
        out: "total = 0.00M used = 0.00M free = 0.00M",
      },
    },
    ...extra,
  };
}
test("health classifies running, oneshot and idle socket without retaining raw output", () => {
  const r = invoke(health());
  assert(r.snapshot.healthy);
  assert(r.snapshot.ready);
  assert.equal(r.snapshot.services.app.lastExit, 75);
  assert.equal(r.snapshot.services.ssh.healthy, true);
  assert.equal(r.snapshot.swapUsed, "0.00M");
  assert.deepEqual(r.snapshot.memoryPages, {
    "Pages free": 42,
    "Pages active": 99,
    "Pages wired down": 10,
    "Pages occupied by compressor": 11,
  });
  assert(r.snapshot.metricsAvailable);
  assert(!JSON.stringify(r.snapshot).match(/private|mock-sensitive/));
  assert(r.calls.every((c: any) => c.timeout === 5000));
  assert(!Number.isNaN(Date.parse(r.snapshot.time)));
});
test("failed readiness, missing socket and failed/running oneshots are unhealthy", () => {
  for (const [key, out, code] of [
    ["wait --timeout 0", "private failure", 1],
    ["/bin/launchctl print system/ssh", "", 113],
    [
      "/bin/launchctl print system/sops",
      "state = not running\nlast exit code = 1\n",
      0,
    ],
    [
      "/bin/launchctl print system/sops",
      "state = running\nlast exit code = 0\n",
      0,
    ],
  ] as const) {
    const input = health();
    input.responses[key] = { code, out };
    assert.equal(invoke(input).snapshot.healthy, false);
  }
});
test("metrics failure is separate from readiness; unknown state and Unicode integers remain typed", () => {
  const input = health();
  input.responses["/usr/bin/vm_stat"] = { code: 1, out: "Pages free: ٧.\n" };
  input.responses["/usr/sbin/sysctl -n vm.swapusage"] = {
    code: 1,
    out: "not numeric",
  };
  input.responses["/bin/launchctl print system/sops"] = {
    code: 0,
    out: "last exit code = -٠\n",
  };
  const r = invoke(input).snapshot;
  assert(r.healthy);
  assert.equal(r.services.sops.state, "unknown");
  assert.equal(r.services.sops.lastExit, 0);
  assert.equal(r.memoryPages["Pages free"], 7);
  assert.equal(r.swapUsed, null);
  assert.equal(r.metricsAvailable, false);
});
test("duplicate labels aggregate the final service record", () => {
  const input = health();
  input.config.services = [
    { label: "app", kind: "running" },
    { label: "app", kind: "socket" },
  ];
  input.responses["/bin/launchctl print system/app"].out =
    "state = not running\n";
  assert.equal(invoke(input).snapshot.healthy, true);
});
test("copytruncate preserves the writer inode and two private bounded archives", () => {
  const p = join(directory(), "service.log");
  const fd = fs.openSync(p, "a");
  const inode = fs.fstatSync(fd).ino;
  for (const char of ["A", "B", "C"]) {
    fs.writeSync(fd, char.repeat(20));
    assert.equal(invoke({ op: "rotate", path: p, limit: 10 }).rotated, 1);
    assert.equal(fs.statSync(p).ino, inode);
  }
  fs.writeSync(fd, "live");
  fs.closeSync(fd);
  assert.equal(fs.readFileSync(p, "utf8"), "live");
  assert.equal(fs.readFileSync(p + ".1", "utf8"), "C".repeat(10));
  assert.equal(fs.readFileSync(p + ".2", "utf8"), "B".repeat(10));
  assert.equal(fs.statSync(p + ".1").mode & 0o777, 0o600);
});
test("rotation skips missing/small logs and rejects symlinks, directories and FIFOs", () => {
  const dir = directory(),
    p = join(dir, "log"),
    link = join(dir, "link"),
    fifo = join(dir, "fifo");
  assert.equal(invoke({ op: "rotate", path: p, limit: 10 }).rotated, 0);
  fs.writeFileSync(p, "small");
  assert.equal(invoke({ op: "rotate", path: p, limit: 10 }).rotated, 0);
  fs.symlinkSync(p, link);
  assert.equal(spawnSync("mkfifo", [fifo]).status, 0);
  for (const path of [dir, link, fifo])
    assert.equal(invoke({ op: "rotate", path, limit: 1 }).rotated, -1);
  assert.equal(fs.readFileSync(p, "utf8"), "small");
});
test("failed archive publication never truncates the live log", () => {
  const p = join(directory(), "log");
  fs.writeFileSync(p, "retain this");
  fs.writeFileSync(p + ".1", "old");
  fs.mkdirSync(p + ".2");
  assert.equal(invoke({ op: "rotate", path: p, limit: 3 }).rotated, -1);
  assert.equal(fs.readFileSync(p, "utf8"), "retain this");
});
test("record publishes private JSON atomically and preserves prior status on retention failure", () => {
  const dir = directory(),
    log = join(dir, "log"),
    status = join(dir, "status");
  fs.writeFileSync(log, "long long log");
  const input = health();
  Object.assign(input.config, {
    logs: [log],
    logLimitBytes: 5,
    statusFile: status,
  });
  Object.assign(input, { record: true });
  const result = invoke(input);
  assert(result.ok);
  assert.deepEqual(
    JSON.parse(fs.readFileSync(status, "utf8")),
    result.snapshot,
  );
  assert.equal(fs.statSync(status).mode & 0o777, 0o600);
  assert.equal(result.snapshot.rotated, 1);
  const previous = fs.readFileSync(status);
  fs.unlinkSync(log);
  fs.symlinkSync(status, log);
  assert.equal(invoke(input).ok, false);
  assert.deepEqual(fs.readFileSync(status), previous);
});
function preflight() {
  const home = directory();
  fs.mkdirSync(join(home, ".ssh"));
  fs.writeFileSync(join(home, ".ssh/id_ed25519"), "fixture, not a key", {
    mode: 0o600,
  });
  return {
    op: "preflight",
    config: {
      architecture: "arm64",
      host: "relay",
      user: "william",
      home,
      legacyUserJobs: ["old.user"],
      legacySystemJobs: ["old.system"],
      requiredFiles: [] as string[],
      executables: [] as string[],
      legacySystemDirectory: join(home, "system"),
      systemSecrets: join(home, "run-secrets"),
      installedConfig: join(home, "installed.json"),
    },
    identity: {
      system: "Darwin",
      machine: "arm64",
      found: true,
      uid: process.getuid!(),
      home,
    },
    responses: {
      "/bin/hostname -s": { code: 0, out: " RELAY\n" },
      "/usr/bin/defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser":
        { code: 1, out: "" },
    } as Record<string, any>,
  };
}
test("clean preflight is read-only and uses ten-second command deadlines", () => {
  const input = preflight(),
    before = fs.readdirSync(input.config.home, { recursive: true });
  const result = invoke(input);
  assert.deepEqual(result.errors, []);
  assert.equal(result.error, false);
  assert.deepEqual(
    fs.readdirSync(input.config.home, { recursive: true }),
    before,
  );
  assert(result.calls.every((c: any) => c.timeout === 10000));
});
test("hostname normalization retains Unicode whitespace stripping", () => {
  const input = preflight();
  input.responses["/bin/hostname -s"].out = "\u0085\u001c RELAY\u2003\u0085";
  assert.deepEqual(invoke(input).errors, []);
});
test("platform, architecture, hostname and auto-login mismatches block", () => {
  const input = preflight();
  input.identity.system = "Linux";
  input.identity.machine = "x86_64";
  input.responses["/bin/hostname -s"].out = "other";
  input.responses[
    "/usr/bin/defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser"
  ].code = 0;
  assert.deepEqual(invoke(input).errors, [
    "Target requires macOS",
    "Target architecture differs from the running machine",
    "Register the correct node identity before deployment",
    "Disable automatic graphical login before deployment",
  ]);
});
test("missing account stops checks early; wrong account home is rejected", () => {
  const input = preflight();
  input.identity.found = false;
  let result = invoke(input);
  assert.deepEqual(result.errors, ["Provision the account before deployment"]);
  assert.equal(result.calls.length, 1);
  input.identity.found = true;
  input.identity.home += "/other";
  assert.deepEqual(invoke(input).errors, [
    "Account home differs from the target",
  ]);
});
test("installed, broken-link and loaded legacy jobs are all blocked", () => {
  const input = preflight(),
    path = join(input.config.home, "Library/LaunchAgents/old.user.plist");
  fs.mkdirSync(resolve(path, ".."), { recursive: true });
  fs.symlinkSync("/missing", path);
  input.responses["/bin/launchctl print system/old.system"] = {
    code: 0,
    out: "private environment",
  };
  assert.deepEqual(invoke(input).errors, [
    "Retire legacy user job: old.user",
    "Retire legacy system job: old.system",
  ]);
});
test("private key type, permissions and ownership, runtime files and execute bits are checked", () => {
  const input = preflight(),
    key = join(input.config.home, ".ssh/id_ed25519");
  fs.chmodSync(key, 0o644);
  input.config.requiredFiles = [join(input.config.home, "model")];
  input.config.executables = [key];
  assert.equal(invoke(input).errors.length, 3);
  fs.chmodSync(key, 0o700);
  input.config.requiredFiles = [];
  input.config.executables = [];
  input.identity.uid += 1;
  assert.deepEqual(invoke(input).errors, [
    "Shared SSH key must be private and user-owned",
  ]);
  fs.unlinkSync(key);
  assert.deepEqual(invoke(input).errors, [
    "Provision the approved shared identity before deployment",
  ]);
  fs.mkdirSync(key);
  assert.deepEqual(invoke(input).errors, [
    "Shared SSH key must be private and user-owned",
  ]);
});
test("unmanaged secret directories and system-secret ownership cannot be adopted", () => {
  const input = preflight();
  fs.mkdirSync(join(input.config.home, ".config/sops-nix/secrets"), {
    recursive: true,
  });
  fs.mkdirSync(input.config.systemSecrets);
  assert.deepEqual(invoke(input).errors, [
    "Inspect the unmanaged secret directory before migration",
    "Existing system secrets are not owned by this profile",
  ]);
});
test("preflight spawn/timeout failures stop before later commands", () => {
  const input = preflight();
  input.responses["/bin/hostname -s"] = { code: 124, error: true, out: "" };
  const r = invoke(input);
  assert(r.error);
  assert.equal(r.calls.length, 1);
});
test("policy diff is sorted, ignores unknown installed fields and never displays their values", () => {
  const input = preflight();
  fs.writeFileSync(
    input.config.installedConfig,
    JSON.stringify({
      ...input.config,
      host: "old",
      privateEnvironment: { TOKEN: "mock-sensitive-data" },
    }),
  );
  const diff = invoke({ op: "diff", config: input.config }).diff;
  assert(diff.includes("--- installed server policy"));
  assert(diff.includes("+++ candidate server policy"));
  assert(diff.includes('-  "host": "old"'));
  assert(diff.includes('+  "host": "relay"'));
  assert(
    !diff.includes("mock-sensitive") && !diff.includes("privateEnvironment"),
  );
  fs.writeFileSync(
    input.config.installedConfig,
    JSON.stringify({ ...input.config, ignored: "mock-sensitive-data" }),
  );
  assert.equal(invoke({ op: "diff", config: input.config }).diff, "");
  for (const invalid of ["broken json", "null", "123"]) {
    fs.writeFileSync(input.config.installedConfig, invalid);
    assert.equal(invoke({ op: "diff", config: input.config }).ok, false);
  }
});
function deployment(mode = "") {
  const dir = directory(),
    target = join(dir, "new"),
    old = join(dir, "old"),
    other = join(dir, "other"),
    profile = join(dir, "profile"),
    current = join(dir, "current"),
    homeProfile = join(dir, "home-profile");
  for (const p of [target, old, other]) fs.mkdirSync(p);
  for (const p of [profile, current]) fs.symlinkSync(old, p);
  fs.symlinkSync(join(dir, "missing-home-generation"), homeProfile);
  return {
    op: "deploy",
    mode,
    target,
    old,
    other,
    profile,
    current,
    homeProfile,
    state: join(dir, "state"),
  };
}
const pointer = (p: string) => fs.readlinkSync(p);
function recover(input: any) {
  const dirs = fs
    .readdirSync(input.state)
    .filter((s: string) => s.startsWith("rollout-"));
  assert.equal(dirs.length, 1);
  const file = join(input.state, dirs[0], "generations.json");
  assert.equal(fs.statSync(file).mode & 0o777, 0o600);
  assert.equal(fs.statSync(join(input.state, dirs[0])).mode & 0o777, 0o700);
  return JSON.parse(fs.readFileSync(file, "utf8"));
}
test("successful deployment double-checks preflight before registration and saves paired recovery", () => {
  const input = deployment(),
    r = invoke(input);
  assert(r.ok);
  assert.equal(r.calls.length, 4);
  assert.equal(r.calls[0].stateExists, false);
  assert.equal(r.calls[1].stateExists, true);
  assert(r.calls[2].args[0].endsWith("/nix-env"));
  assert.deepEqual(recover(input), {
    target: input.target,
    previousProfile: input.old,
    previousActive: input.old,
    previousHome: join(
      resolve(input.homeProfile, ".."),
      "missing-home-generation",
    ),
  });
  assert.equal(pointer(input.profile), input.target);
  assert.equal(pointer(input.current), input.target);
  assert.equal(fs.statSync(input.state).mode & 0o777, 0o700);
  assert.equal(
    fs.statSync(join(input.state, "deploy.lock")).mode & 0o777,
    0o600,
  );
});
test("first preflight failure creates no state and never touches the profile", () => {
  const input = deployment("preflight"),
    r = invoke(input);
  assert.equal(r.ok, false);
  assert.equal(r.calls.length, 1);
  assert.equal(fs.existsSync(input.state), false);
  assert.equal(pointer(input.profile), input.old);
});
test("second preflight failure and lock contention create no recovery or registration", () => {
  for (const mode of ["preflight-locked", "locked"]) {
    const input = deployment(mode),
      r = invoke(input);
    assert.equal(r.ok, false);
    assert.equal(r.calls.length, mode === "locked" ? 1 : 2);
    assert.deepEqual(fs.readdirSync(input.state), ["deploy.lock"]);
    assert.equal(pointer(input.profile), input.old);
  }
});
test("failed activation and partial registration restore the previous profile only", () => {
  for (const mode of ["activation", "registration", "no-activation"]) {
    const input = deployment(mode),
      r = invoke(input);
    assert.equal(r.ok, false);
    assert.equal(pointer(input.profile), input.old);
    assert.equal(pointer(input.current), input.old);
    assert.equal(recover(input).previousProfile, input.old);
    assert(
      r.messages.some((m: string) => m.includes("services may have changed")),
    );
  }
});
test("concurrent profile changes survive failed and successful activation exits", () => {
  for (const mode of ["race", "race-success"]) {
    const input = deployment(mode),
      r = invoke(input);
    assert.equal(r.ok, false);
    assert.equal(pointer(input.profile), input.other);
    assert(r.messages.some((m: string) => m.includes("refusing to overwrite")));
    assert.equal(
      r.calls.filter((c: any) => c.args[0].endsWith("/nix-env")).length,
      1,
    );
  }
});
test("failed first install removes only its newly registered profile pointer", () => {
  const input = deployment("activation");
  fs.unlinkSync(input.profile);
  const r = invoke(input);
  assert.equal(r.ok, false);
  assert.equal(fs.existsSync(input.profile), false);
  assert(fs.existsSync(input.target));
  assert.equal(recover(input).previousProfile, null);
});
test("failed recovery never reports successful rollback or changes active generation", () => {
  const input = deployment("rollback"),
    r = invoke(input);
  assert.equal(r.ok, false);
  assert.equal(pointer(input.profile), input.target);
  assert.equal(pointer(input.current), input.old);
  assert.equal(r.calls.at(-1).args.at(-1), input.old);
  assert.equal(r.messages.length, 1);
  recover(input);
});
test("captured commands normalize newlines, suppress stderr, return failure and time out", () => {
  let r = invoke({
    op: "command",
    args: [
      "/bin/sh",
      "-c",
      "printf 'one\\r\\ntwo\\r'; printf 'private-error' >&2; exit 7",
    ],
    timeout: 1000,
  });
  assert.equal(r.code, 7);
  assert.equal(r.out, "one\ntwo\n");
  assert.equal(r.error, false);
  r = invoke({
    op: "command",
    args: ["/bin/sh", "-c", 'IFS= read -r line; printf "%s" "$line"'],
    stdin: "inherited input\n",
    timeout: 1000,
  });
  assert.equal(r.out, "inherited input");
  assert.equal(r.code, 0);
  const start = Date.now();
  r = invoke({
    op: "command",
    args: ["/bin/sh", "-c", "exec sleep 20"],
    timeout: 30,
  });
  assert(r.error);
  assert.equal(r.code, 124);
  assert(Date.now() - start < 3000);
  r = invoke({
    op: "command",
    args: ["/missing/darwin-fixture-command"],
    timeout: 100,
  });
  assert(r.error);
  assert.equal(r.code, 124);
});
test("public CLI retains usage, store-path and explicit root gates without writing", () => {
  for (const name of ["health", "preflight", "deploy"])
    assert.equal(spawnSync(join(bin, "darwin-" + name), ["--help"]).status, 0);
  assert.notEqual(process.getuid!(), 0, "run this manual suite without root");
  let r = spawnSync(join(bin, "darwin-deploy"), [root], { encoding: "utf8" });
  assert.equal(r.status, 2);
  assert(r.stderr.includes("explicit sudo authorization"));
  const input = preflight(),
    file = join(root, "cli-config");
  fs.writeFileSync(file, JSON.stringify(input.config));
  r = spawnSync(
    join(bin, "darwin-preflight"),
    ["--config", file, "--target", root],
    { encoding: "utf8" },
  );
  assert.equal(r.status, 2);
  assert(r.stderr.includes("already-built Nix store path"));
  const status = join(root, "not-recorded");
  fs.writeFileSync(
    file,
    JSON.stringify({
      services: [],
      readiness: ["/bin/sh", "-c", "exit 0"],
      logs: [],
      logLimitBytes: 5,
      statusFile: status,
    }),
  );
  r = spawnSync(join(bin, "darwin-health"), ["--config", file, "--record"], {
    encoding: "utf8",
  });
  assert.equal(r.status, 2, r.stderr);
  assert(r.stderr.includes("require root"));
  assert.equal(fs.existsSync(status), false);
});
