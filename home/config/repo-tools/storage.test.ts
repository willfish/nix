import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, test } from "node:test";
const binary = process.env.STORAGE_REPORT_BIN!;
assert(binary, "set STORAGE_REPORT_BIN");
const roots: string[] = [];
afterEach(() => {
  for (const p of roots.splice(0))
    fs.rmSync(p, { recursive: true, force: true });
});
function fixture() {
  const root = fs.mkdtempSync(join(tmpdir(), "storage-report-"));
  roots.push(root);
  const bin = join(root, "bin"),
    flake = join(root, "selected flake"),
    home = join(root, "home");
  fs.mkdirSync(bin);
  fs.mkdirSync(flake);
  const script = join(home, ".dotfiles/scripts/nix-storage-costs");
  fs.mkdirSync(join(script, ".."), { recursive: true });
  fs.copyFileSync("scripts/nix-storage-costs", script);
  fs.chmodSync(script, 0o755);
  const env: any = { ...process.env };
  for (const key of Object.keys(env))
    if (key.startsWith("NH_")) delete env[key];
  Object.assign(env, {
    HOME: home,
    XDG_STATE_HOME: join(root, "state"),
    PATH: bin + ":" + process.env.PATH,
    STORAGE_CALLS: join(root, "calls.jsonl"),
  });
  function tool(name: string, body: string) {
    fs.writeFileSync(
      join(bin, name),
      "#!/usr/bin/env bash\nset -eu\n" + body + "\n",
      { mode: 0o755 },
    );
  }
  tool("uname", 'printf "%s\\n" "${STORAGE_PLATFORM:-Linux}"');
  tool("hostname", 'printf "%s\\n" "${STORAGE_HOST:-andromeda}"');
  tool("whoami", 'printf "william\\n"');
  fs.symlinkSync(binary, join(bin, "nix-storage-report"));
  for (const name of ["nix", "nix-store", "nh"])
    fs.writeFileSync(
      join(bin, name),
      `#!${process.execPath}\nconst fs=require('node:fs'),p=require('node:path');const tool=p.basename(process.argv[1]),args=process.argv.slice(2);if(tool==='nix'&&process.env.STORAGE_NIX_STATUS)process.exit(Number(process.env.STORAGE_NIX_STATUS));fs.appendFileSync(process.env.STORAGE_CALLS,JSON.stringify({tool,args,flake:process.env.NIX_STORAGE_COST_FLAKE,scope:process.env.NIX_STORAGE_COST_SCOPE})+'\\n');const data=JSON.parse(process.env.STORAGE_DATA||'{}');if(tool==='nh')process.exit(Number(process.env.NH_FIXTURE_STATUS||0));if(tool==='nix-store'){if(args[0]==='--query'){if(args.at(-1).includes('missing'))process.exit(1);console.log('hash');}else console.log((data.closures?.[args.at(-1)]||[args.at(-1)]).join('\\n'));}else if(args[0]==='build')console.log(args.some(s=>s.includes('homeConfigurations'))?'/nix/store/fixture-home':'/nix/store/fixture-system');else if(args[0]==='eval')console.log(args.includes('--json')?(process.env.STORAGE_INVENTORY||'[]'):(process.env.STORAGE_SELECTED_HOME||'william-linux'));else if(args[0]==='path-info'){if(!args.includes('--json-format')||args[args.indexOf('--json-format')+1]!=='1')process.exit(22);console.log(JSON.stringify(Object.fromEntries(args.filter(s=>s.startsWith('/nix/store/')).map(s=>[s,{narSize:data.sizes?.[s]??5,closureSize:7}]))));}else process.exit(23);\n`,
      { mode: 0o755 },
    );
  function calls(toolName = "nix") {
    return fs.existsSync(env.STORAGE_CALLS)
      ? fs
          .readFileSync(env.STORAGE_CALLS, "utf8")
          .trim()
          .split("\n")
          .filter(Boolean)
          .map((s) => JSON.parse(s))
          .filter((x) => x.tool === toolName)
      : [];
  }
  function run(...args: string[]) {
    return spawnSync("bash", [script, ...args], {
      cwd: root,
      env,
      encoding: "utf8",
      timeout: 15000,
    });
  }
  function builds() {
    return calls().filter((c) => c.args[0] === "build");
  }
  function one(fragment: string) {
    assert.equal(builds().length, 1);
    assert(builds()[0].args.includes(flake + fragment));
  }
  return { root, bin, flake, home, script, env, tool, calls, run, builds, one };
}
function ok(result: any) {
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
}
test("home and system scopes build only the selected configuration", () => {
  let f = fixture();
  ok(f.run("--scope", "home", "--flake", f.flake, "--home", "william-darwin"));
  f.one('#homeConfigurations."william-darwin".activationPackage');
  const md = fs.readFileSync(
    join(
      f.root,
      "state/nix-storage-costs/andromeda-william-darwin-storage-costs.md",
    ),
    "utf8",
  );
  assert(!md.includes("NixOS system:"));
  f = fixture();
  ok(f.run("--scope", "system", "--flake", f.flake, "--host", "foundation"));
  f.one('#nixosConfigurations."foundation".config.system.build.toplevel');
});
test("Darwin defaults to home while direct Linux defaults to both", () => {
  let f = fixture();
  Object.assign(f.env, {
    STORAGE_PLATFORM: "Darwin",
    STORAGE_HOST: "relay",
    STORAGE_SELECTED_HOME: "william-darwin",
  });
  ok(f.run("--flake", f.flake));
  f.one('#homeConfigurations."william-darwin".activationPackage');
  f = fixture();
  ok(f.run("--flake", f.flake, "--home", "william-linux"));
  assert.equal(f.builds().length, 2);
  assert(f.builds().every((c) => c.flake === f.flake));
});
test("nh flags and scoped environment select the intended flake and configuration", () => {
  let f = fixture();
  ok(
    f.run(
      "--after-nh-switch",
      "home",
      "switch",
      "--diff",
      "always",
      "--configuration=william-darwin",
      f.flake,
      "--cores",
      "2",
    ),
  );
  f.one('#homeConfigurations."william-darwin".activationPackage');
  f = fixture();
  f.env.NH_OS_FLAKE = f.flake;
  ok(f.run("--after-nh-switch", "os", "switch", "--hostname=foundation"));
  f.one('#nixosConfigurations."foundation".config.system.build.toplevel');
  f = fixture();
  Object.assign(f.env, { NH_HOME_FLAKE: f.flake, NH_FLAKE: "/wrong/flake" });
  ok(f.run("--after-nh-switch", "home", "switch", "-c", "william-linux"));
  f.one('#homeConfigurations."william-linux".activationPackage');
});
test("dry, help, missing flake and ambiguous callbacks never call Nix", () => {
  for (const args of [
    ["--dry"],
    ["-n"],
    ["--help"],
    ["-h"],
    ["--file", "/unrelated/home.nix"],
    ["-c", "william-linux"],
  ]) {
    const f = fixture();
    const selection = ["--dry", "-n", "--help", "-h"].includes(args[0])
      ? [f.flake, ...args]
      : args;
    ok(f.run("--after-nh-switch", "home", "switch", ...selection));
    assert.deepEqual(f.calls(), []);
  }
});
test("resolved store outputs and input overrides are skipped rather than reinterpreted", () => {
  let f = fixture();
  const output = join(f.root, "realized-output"),
    link = join(f.root, "result");
  fs.mkdirSync(output);
  fs.symlinkSync(output, link);
  f.tool("realpath", 'printf "/nix/store/fixture-system\\n"');
  const result = f.run("--after-nh-switch", "os", "switch", link);
  ok(result);
  assert(result.stderr.includes("Storage-cost callback skipped"));
  assert.deepEqual(f.calls(), []);
  for (const extra of [
    ["--override-input", "nixpkgs", "/other"],
    ["--", "--override-input", "name", "/other"],
  ]) {
    f = fixture();
    ok(
      f.run(
        "--after-nh-switch",
        "home",
        "switch",
        f.flake,
        "-c",
        "william-linux",
        ...extra,
      ),
    );
    assert.deepEqual(f.calls(), []);
  }
});
test("an empty Nix argument separator remains allowed", () => {
  const f = fixture();
  ok(
    f.run(
      "--after-nh-switch",
      "home",
      "switch",
      f.flake,
      "-c",
      "william-linux",
      "--",
    ),
  );
  f.one('#homeConfigurations."william-linux".activationPackage');
});
test("inventory expression does not force unselected configurations", () => {
  const source = fs.readFileSync("scripts/nix-storage-costs", "utf8"),
    expressions = [...source.matchAll(/--expr '(.*?)'/gs)];
  assert.equal(expressions.length, 2);
  const inventory = expressions[1][1].replace(
    'builtins.getFlake (builtins.getEnv "NIX_STORAGE_COST_FLAKE")',
    "fixture",
  );
  const library =
    "rec { optionals = condition: values: if condition then values else []; concatStringsSep = builtins.concatStringsSep; attrByPath = path: fallback: attrs: if path == [] then attrs else if builtins.hasAttr (builtins.head path) attrs then attrByPath (builtins.tail path) fallback attrs.${builtins.head path} else fallback; }";
  const home =
      "{ fixture.config = { home.packages = []; programs = { git.package = null; fish.package = null; zoxide.package = null; direnv.package = null; }; }; }",
    system =
      "{ fixture.config = { environment = { systemPackages = []; shells = []; }; users.users.william.packages = []; users.defaultUserShell = null; programs.fish.package = null; virtualisation.docker.package = null; boot.kernelPackages.kernel = null; fonts.packages = []; }; }";
  for (const scope of ["home", "system"]) {
    const expression = `let fixture = { inputs.nixpkgs.lib = ${library}; homeConfigurations = ${scope === "home" ? home : 'throw "unselected home"'}; nixosConfigurations = ${scope === "system" ? system : 'throw "unselected system"'}; }; in ${inventory}`;
    const result = spawnSync(
      "nix-instantiate",
      ["--eval", "--strict", "--json", "--expr", expression],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          NIX_STORAGE_COST_SCOPE: scope,
          NIX_STORAGE_COST_HOST: "fixture",
          NIX_STORAGE_COST_HOME: "fixture",
        },
        timeout: 15000,
      },
    );
    assert.equal(ok(result).trim(), "[]");
  }
});
function shellCode(shell: string) {
  const source = fs.readFileSync("home/user/shells.nix", "utf8");
  if (shell === "bash") {
    const matches = [...source.matchAll(/initExtra = ''\n(.*?)\n    '';/gs)];
    assert.equal(matches.length, 1);
    return matches[0][1].replaceAll("''${", "${");
  }
  return ["__storage_costs_after_nh_switch", "nh"]
    .map((name) => {
      const matches = [
        ...source.matchAll(
          new RegExp("      " + name + " = ''\\n(.*?)\\n      '';", "gs"),
        ),
      ];
      assert.equal(matches.length, 1);
      return "function " + name + "\n" + matches[0][1] + "\nend\n";
    })
    .join("");
}
function shellRun(
  f: ReturnType<typeof fixture>,
  shell: string,
  args: string[],
) {
  fs.rmSync(f.env.STORAGE_CALLS, { force: true });
  const path = join(f.root, "source." + shell);
  fs.writeFileSync(path, shellCode(shell));
  const command =
    shell === "bash"
      ? [
          "--noprofile",
          "--norc",
          "-c",
          'source "$1"; shift; nh "$@"',
          "fixture",
        ]
      : ["--no-config", "-c", "source $argv[1]; nh $argv[2..-1]"];
  return spawnSync(shell, [...command, path, ...args], {
    cwd: f.root,
    env: f.env,
    encoding: "utf8",
    timeout: 15000,
  });
}
test("Bash and Fish preserve nh arguments, switch status and optional report failure", () => {
  const f = fixture(),
    args = [
      "home",
      "switch",
      "--diff",
      "always",
      f.flake,
      "-c",
      "william-darwin",
    ];
  for (const shell of ["bash", "fish"]) {
    ok(shellRun(f, shell, args));
    assert.deepEqual(f.calls("nh")[0].args, args);
    f.one('#homeConfigurations."william-darwin".activationPackage');
  }
  f.env.NH_FIXTURE_STATUS = "7";
  for (const shell of ["bash", "fish"]) {
    assert.equal(shellRun(f, shell, ["home", "switch", f.flake]).status, 7);
    assert.deepEqual(f.calls(), []);
  }
  f.env.NH_FIXTURE_STATUS = "0";
  f.env.STORAGE_NIX_STATUS = "19";
  for (const shell of ["bash", "fish"]) ok(shellRun(f, shell, args));
});
test("shared closure accounting, deduplication, grouped names, skipped roots and CSV agree", () => {
  const f = fixture();
  const item = (path: string, name: string, source: string, label: string) => ({
    storePath: "/nix/store/" + path,
    name,
    pname: name,
    source,
    label,
  });
  f.env.STORAGE_INVENTORY = JSON.stringify([
    item("A", "Alpha", "nixos", "one"),
    item("A", "Alpha", "home-manager", "two"),
    item("B", "Beta", "nixos", "three"),
    item("C", "Alpha", "home-manager", "four"),
    item("missing", "Unrealized", "home-manager", "five"),
  ]);
  f.env.STORAGE_DATA = JSON.stringify({
    closures: {
      "/nix/store/A": ["/nix/store/A", "/nix/store/shared", "/nix/store/X"],
      "/nix/store/B": ["/nix/store/B", "/nix/store/shared"],
      "/nix/store/C": ["/nix/store/C", "/nix/store/shared", "/nix/store/Z"],
    },
    sizes: {
      "/nix/store/A": 100,
      "/nix/store/B": 200,
      "/nix/store/C": 50,
      "/nix/store/shared": 1000,
      "/nix/store/X": 300,
      "/nix/store/Z": 700,
    },
  });
  const result = f.run(
    "--scope",
    "home",
    "--flake",
    f.flake,
    "--home",
    "william@fixture",
    "--summary",
    "--color",
    "never",
  );
  const out = ok(result);
  assert(out.includes("inventory_roots=3"));
  assert(out.includes("skipped_unrealized_roots=1"));
  assert(out.includes("inventory_closure=2.3 KiB"));
  assert(result.stderr.includes("skipping 1 unrealized"));
  const prefix = join(
    f.root,
    "state/nix-storage-costs/andromeda-william_fixture-storage-costs",
  );
  const csv = fs.readFileSync(prefix + ".csv", "utf8");
  assert(
    csv.includes(
      "Alpha,Alpha,home-manager,four,/nix/store/C,50,1750,750,3,2\r\n",
    ),
  );
  assert(
    csv.includes(
      'Alpha,Alpha,"home-manager,nixos","one,two",/nix/store/A,100,1400,400,3,2\r\n',
    ),
  );
  const md = fs.readFileSync(prefix + ".md", "utf8");
  assert(
    md.includes(
      "| `Alpha` | 2 | `home-manager,nixos` | 1.1 KiB | 2.1 KiB | 150 B |",
    ),
  );
  assert(!out.includes("\x1b"));
  assert(
    ok(
      f.run(
        "--scope",
        "home",
        "--flake",
        f.flake,
        "--home",
        "william@fixture",
        "--summary",
        "--color",
        "always",
      ),
    ).includes("\x1b[1;36m"),
  );
});
