import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
  rmSync,
  existsSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
const bin = process.env.PI_BUS_HOST_BIN!,
  argvFixture = process.env.PI_BUS_HOST_ARGV!,
  legacy = process.env.PI_BUS_HOST_LEGACY,
  python = process.env.PI_BUS_HOST_PYTHON || "python3";
assert.ok(bin && argvFixture, "Set PI_BUS_HOST_BIN and PI_BUS_HOST_ARGV");
function run(url: string | undefined, old = false) {
  const env = { ...process.env };
  if (url === undefined) delete env.PI_AGENT_BUS_URL;
  else env.PI_AGENT_BUS_URL = url;
  const r = spawnSync(old ? python : bin, old ? [legacy!] : [], {
    env,
    encoding: "utf8",
    timeout: 10000,
  });
  assert.ifError(r.error);
  assert.equal(r.status, 0, r.stderr);
  assert.equal(r.stderr, "");
  return r.stdout;
}
function check(url: string | undefined, host: string) {
  assert.equal(run(url), host + "\n", JSON.stringify(url));
  if (legacy) assert.equal(run(url, true), host + "\n", JSON.stringify(url));
}
test("unset and empty URLs use the effective client default; schemes and DNS case stay exact", () => {
  for (const url of [
    undefined,
    "",
    "http://terminus:7420",
    "https://TERMINUS:7420/",
  ])
    check(url, "terminus");
  for (const scheme of ["HTTP", "Https", "httpS", "ftp", "ws"])
    check(scheme + "://terminus:7420", "");
  check("http://bus.EXAMPLE:7420/base", "bus.example");
});
test("label boundaries and total host spelling match the original rather than a broad URL parser", () => {
  for (const host of [
    "a",
    "9z",
    "a-9",
    "a".repeat(63),
    "a".repeat(63) + "." + ("b".repeat(63) + ".").repeat(4) + "z",
    "example.0xz",
    "0x.example",
  ])
    check("http://" + host, host);
  for (const host of [
    ".",
    "a.",
    ".a",
    "a..b",
    "-a",
    "a-",
    "a.-b",
    "a.b-",
    "a".repeat(64),
    "a_b",
    "bücher.example",
    "尾.example",
    "xn--",
    "[::1]",
  ])
    check("http://" + host, "");
});
test("only canonical dotted IPv4 is accepted when the final label looks decimal or hexadecimal", () => {
  for (const host of ["0.0.0.0", "127.0.0.1", "255.255.255.255", "192.168.1.9"])
    check("http://" + host + ":7420", host);
  for (const host of [
    "127.1",
    "2130706433",
    "0177.0.0.1",
    "127.00.0.1",
    "0x7f000001",
    "0X7F000001",
    "127.0.0.256",
    "256.0.0.1",
    "1.2.3.4.5",
    "bus.123",
    "bus.0x",
    "bus.0xabc",
    "0",
    "1",
    "0.0.0.00",
    "1.2.3.999999999999999999999999",
  ])
    check("http://" + host, "");
});
test("ports retain the existing ASCII-digit grammar without adding a numeric range restriction", () => {
  for (const port of ["0", "00080", "65535", "65536", "9".repeat(200)])
    check("https://bus.example:" + port + "/p", "bus.example");
  for (const port of ["", "-1", "+80", "0x50", "８０", "1:2", "80.0", " 80"])
    check("http://bus.example:" + port, "");
});
test("paths accept Unicode and percent text but reject Python whitespace, raw queries, fragments and backslashes", () => {
  for (const path of [
    "",
    "/",
    "//x",
    "/尾🙂",
    "/%3f%23%20",
    "/\u200b",
    "/\u2060",
    "/\x1b",
    "/a:b@c",
  ])
    check("http://bus.example:7420" + path, "bus.example");
  for (const c of [
    " ",
    "\t",
    "\n",
    "\r",
    "\v",
    "\f",
    "\x1c",
    "\x1d",
    "\x1e",
    "\x1f",
    "\x85",
    "\xa0",
    "\u1680",
    "\u2000",
    "\u2001",
    "\u2002",
    "\u2003",
    "\u2004",
    "\u2005",
    "\u2006",
    "\u2007",
    "\u2008",
    "\u2009",
    "\u200a",
    "\u2028",
    "\u2029",
    "\u202f",
    "\u205f",
    "\u3000",
    "?",
    "#",
    "\\",
  ])
    check("http://bus.example/a" + c + "b", "");
});
test("credentials and ambiguous authority spellings fail silently without disclosing the URL", () => {
  for (const url of [
    " http://bus",
    "http://bus ",
    "http://bus\n",
    "http://user:fake-pass@bus:7420",
    "http://user@bus",
    "http://bad,host",
    "http://bus\\@127.1",
    "http://[bad",
    "http:///bus",
    "//bus",
    "http://%62us",
    "http://bus?query",
    "http://bus#fragment",
    "http://bus:80?query",
  ])
    check(url, "");
});
test("deterministic authority corpus agrees with the retained regex and IPv4 reference", () => {
  let seed = 37;
  const chars = "abcXYZ09.-_:@/\\?#% ";
  for (let i = 0; i < 180; i++) {
    let text = "";
    const length = 1 + (i % 30);
    for (let j = 0; j < length; j++) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      text += chars[seed % chars.length];
    }
    const url = "http://" + text;
    const actual = run(url);
    assert.match(actual, /^(?:[a-z0-9.-]+)?\n$/);
    if (legacy) assert.equal(actual, run(url, true), url);
  }
});
test("invalid UTF-8 environment bytes preserve surrogateescape path behavior but never become host characters", () => {
  for (const [prefix, suffix, expected] of [
    ["http://bus.example/path-", "", "bus.example\n"],
    ["http://bus-", ".example", "\n"],
  ] as [string, string, string][])
    for (const old of legacy ? [false, true] : [false]) {
      const bytes = Buffer.concat([
        Buffer.from(prefix),
        Buffer.from([255, 0xed, 0xa0, 0x80]),
        Buffer.from(suffix),
      ]);
      const r = spawnSync(
        argvFixture,
        old
          ? [bytes.toString("hex"), python, legacy!]
          : [bytes.toString("hex"), bin],
        { encoding: "utf8", timeout: 10000 },
      );
      assert.equal(r.status, 0, r.stderr);
      assert.equal(r.stdout, expected);
      assert.equal(r.stderr, "");
    }
});

const wrappers = process.env.PI_BUS_HOST_WRAPPERS
  ? JSON.parse(readFileSync(process.env.PI_BUS_HOST_WRAPPERS, "utf8"))
  : null;
function script(path: string, content: string) {
  writeFileSync(path, content);
  chmodSync(path, 0o700);
  return path;
}
function launch(
  options: {
    env?: Record<string, string>;
    args?: string[];
    public?: boolean;
    parserFailure?: boolean;
    disabled?: boolean;
  } = {},
) {
  const root = mkdtempSync(join(tmpdir(), "pi-bus-wrapper-"));
  try {
    const pi = script(
      join(root, "pi"),
      `#!${process.execPath}\nconst keys=['PI_AGENT_BUS_URL','PI_AGENT_BUS_TOKEN','PI_AGENT_BUS_ENABLED','PI_AGENT_BUS_CONTROL','PI_AGENT_BUS_OPERATOR_READ','PI_AGENT_BUS_OPERATOR_HISTORY','TYPESAFE_API_KEY','NO_PROXY','no_proxy','CAPTURE_BRANCH'];console.log(JSON.stringify({args:process.argv.slice(2),env:Object.fromEntries(keys.map(k=>[k,process.env[k]??null]))}));\n`,
    );
    const capture = script(
      join(root, "capture"),
      '#!/usr/bin/env bash\n[[ $1 == pi && $2 == -- ]] || exit 73\nshift 2\nexport CAPTURE_BRANCH=1\nexec "$@"\n',
    );
    const secret = script(
      join(root, "secret"),
      '#!/usr/bin/env bash\nprintf "%s\\n" "${1##*/}" >> "$FIXTURE_READS"\nprintf "%s\\n" fixture-secret\nexit "${SECRET_STATUS:-0}"\n',
    );
    const failed = script(
      join(root, "failed"),
      "#!/usr/bin/env bash\nexit 1\n",
    );
    let text = wrappers[options.public ? "public" : "foundation"];
    assert.match(text, /\/pi-capture-bus-host/);
    assert.doesNotMatch(text, /python/);
    text = text
      .replaceAll(/\/nix\/store\/[^\s"']+\/bin\/read-sops-secret/g, secret)
      .replaceAll(/\/nix\/store\/[^\s"']+\/bin\/prompt-capture/g, capture)
      .replaceAll(
        /\/nix\/store\/[^\s"']+\/bin\/pi-capture-bus-host/g,
        options.parserFailure ? failed : bin,
      )
      .replaceAll(/\/nix\/store\/[^\s"']+\/bin\/pi(?=\s)/g, pi);
    assert.doesNotMatch(
      text,
      /\/nix\/store\/[^\s"']+\/bin\/(?:read-sops-secret|prompt-capture|pi)\s/,
    );
    if (options.disabled) {
      assert.match(text, /bus_enabled=1/);
      text = text.replace("bus_enabled=1", "bus_enabled=0");
    }
    const file = script(join(root, "wrapper"), text);
    const env = {
      PATH: process.env.PATH!,
      HOME: root,
      FIXTURE_READS: join(root, "reads"),
      ...options.env,
    };
    const r = spawnSync("bash", [file, ...(options.args || [])], {
      env,
      encoding: "utf8",
      timeout: 10000,
    });
    assert.ifError(r.error);
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stderr, "");
    return {
      result: JSON.parse(r.stdout),
      reads: existsSync(join(root, "reads"))
        ? readFileSync(join(root, "reads"), "utf8").trim().split("\n")
        : [],
    };
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}
test(
  "rendered private/public wrappers preserve URL spelling and combine both proxy lists only for capture",
  { skip: !wrappers },
  () => {
    for (const publicMode of [false, true])
      for (const url of ["http://BUS.example:7420/base", ""]) {
        const { result } = launch({
          public: publicMode,
          env: {
            CAPTURE_PROMPTS: "1",
            PI_AGENT_BUS_URL: url,
            NO_PROXY: "upper",
            no_proxy: "lower",
          },
          args: ["--test-marker", "value"],
        });
        assert.equal(result.env.PI_AGENT_BUS_URL, url);
        assert.equal(
          result.env.NO_PROXY,
          "upper,lower," + (url ? "bus.example" : "terminus"),
        );
        assert.equal(result.env.no_proxy, result.env.NO_PROXY);
        assert.equal(result.env.CAPTURE_BRANCH, "1");
        assert.deepEqual(result.args.slice(-2), ["--test-marker", "value"]);
      }
    const { result } = launch({
      env: { NO_PROXY: "upper", no_proxy: "lower" },
    });
    assert.equal(result.env.NO_PROXY, "upper");
    assert.equal(result.env.no_proxy, "lower");
    assert.equal(result.env.CAPTURE_BRANCH, null);
  },
);
test(
  "unsupported and failed parsing disables only capture participation and never rewrites caller URL",
  { skip: !wrappers },
  () => {
    for (const url of [
      "http://127.1:7420",
      "http://user@bus",
      "http://[::1]:7420",
      "http://bücher.example:7420",
    ])
      for (const capture of ["0", "1"]) {
        const { result } = launch({
          env: { CAPTURE_PROMPTS: capture, PI_AGENT_BUS_URL: url },
        });
        assert.equal(result.env.PI_AGENT_BUS_URL, url);
        assert.equal(
          result.env.PI_AGENT_BUS_ENABLED,
          capture === "1" ? "0" : null,
        );
        assert.equal(result.env.NO_PROXY, null);
      }
    for (const capture of ["0", "1"]) {
      const { result } = launch({
        env: { CAPTURE_PROMPTS: capture },
        parserFailure: true,
      });
      assert.equal(
        result.env.PI_AGENT_BUS_ENABLED,
        capture === "1" ? "0" : null,
      );
      assert.equal(result.env.CAPTURE_BRANCH, capture === "1" ? "1" : null);
    }
  },
);
test(
  "offline and disabled bus gates still suppress bus-secret reads in both capture paths",
  { skip: !wrappers },
  () => {
    const cases = [
      { args: ["--offline"] },
      { env: { PI_AGENT_BUS_ENABLED: "0" } },
      { disabled: true },
      ...["1", "true", "YES", " true ", "yes "].map((value) => ({
        env: { PI_OFFLINE: value },
      })),
    ];
    for (const capture of ["0", "1"])
      for (const options of cases) {
        const { result, reads } = launch({
          ...options,
          env: { ...options.env, CAPTURE_PROMPTS: capture },
        });
        assert.ok(!reads.includes("PI_AGENT_BUS_TOKEN"));
        assert.equal(result.env.PI_AGENT_BUS_TOKEN, null);
      }
    for (const value of ["0", "false", "no", "", "t rue"]) {
      const { reads } = launch({ env: { PI_OFFLINE: value } });
      assert.equal(reads.filter((x) => x === "PI_AGENT_BUS_TOKEN").length, 1);
    }
  },
);
test(
  "explicit credentials, failed secret reads and public profile boundaries remain unchanged",
  { skip: !wrappers },
  () => {
    for (const capture of ["0", "1"]) {
      let { result, reads } = launch({
        env: {
          CAPTURE_PROMPTS: capture,
          PI_AGENT_BUS_TOKEN: "caller-token",
          TYPESAFE_API_KEY: "caller-key",
        },
      });
      assert.equal(reads.length, 0);
      assert.equal(result.env.PI_AGENT_BUS_TOKEN, "caller-token");
      assert.equal(result.env.TYPESAFE_API_KEY, "caller-key");
      ({ result, reads } = launch({
        env: { CAPTURE_PROMPTS: capture, SECRET_STATUS: "1" },
      }));
      assert.equal(reads.filter((x) => x === "PI_AGENT_BUS_TOKEN").length, 1);
      assert.equal(result.env.PI_AGENT_BUS_TOKEN, null);
      assert.equal(result.env.TYPESAFE_API_KEY, null);
      ({ result, reads } = launch({
        public: true,
        env: { CAPTURE_PROMPTS: capture },
      }));
      assert.equal(reads.length, 0);
      assert.equal(result.env.PI_AGENT_BUS_TOKEN, null);
      assert.equal(result.env.PI_AGENT_BUS_URL, null);
      assert.equal(result.env.NO_PROXY, capture === "1" ? "terminus" : null);
    }
  },
);
