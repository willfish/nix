import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  copyFileSync,
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
import { join, resolve } from "node:path";
import test from "node:test";
const bin = process.env.OPENCODE_ADAPT_BIN!,
  failures = process.env.OPENCODE_ADAPT_FAILURES!,
  legacy = process.env.OPENCODE_ADAPT_LEGACY,
  python = process.env.OPENCODE_ADAPT_PYTHON || "python3";
assert.ok(bin && failures, "Set executable and failure-library paths");
const note = "Load these skills when the task reaches their trigger: ";
function temp(fn: (root: string) => void) {
  const root = mkdtempSync(join(tmpdir(), "opencode-adapt-"));
  try {
    fn(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}
function run(
  src: string,
  dst: string,
  kind = "agent",
  old = false,
  options: any = {},
) {
  const args = ["--kind", kind, src, dst],
    r = spawnSync(old ? python : bin, old ? [legacy!, ...args] : args, {
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
function text(input: string, kind: string, expected?: string) {
  let output: Buffer | undefined;
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "source"),
        dst = join(root, "dest");
      mkdirSync(src);
      writeFileSync(join(src, "sample.md"), input);
      const r = run(src, dst, kind, old);
      assert.equal(r.status, 0, r.stderr);
      const data = readFileSync(join(dst, "sample.md"));
      if (expected !== undefined) assert.equal(data.toString("utf8"), expected);
      if (old) assert.deepEqual(data, output);
      else output = data;
    });
}
function fault(path: string, mode: string) {
  return {
    ...process.env,
    LD_PRELOAD: [process.env.OPENCODE_ADAPT_ASAN_RT, failures]
      .filter(Boolean)
      .join(":"),
    ADAPT_FAILURE_PATH: path,
    ADAPT_FAIL: mode,
  };
}
function tree(path: string) {
  return readdirSync(path, { encoding: "buffer" })
    .map((name) => [
      name.toString("hex"),
      readFileSync(Buffer.concat([Buffer.from(path + "/"), name])).toString(
        "hex",
      ),
    ])
    .sort((a, b) => a[0].localeCompare(b[0]));
}

test("agent allowlist, field order, default mode and skill-note construction retain exact output", () => {
  text(
    "---\nname: builder\ndescription: Do work\ntools: read, bash\nskills: [verification-before-completion]\nmodel: xai/grok-4.7\nthinking: medium\n---\nDo the work.\n",
    "agent",
    "---\ndescription: Do work\nmode: subagent\nmodel: xai/grok-4.7\n---\nDo the work.\n\n" +
      note +
      "verification-before-completion.\n",
  );
});
test("command whitelist and global body-only argument replacement retain raw values", () => {
  text(
    '---\nsubtask: true\nmodel: x/y\nagent: architect\ndescription: Keep $@ here\nargument-hint: "<brief>"\nmode: primary\nskills: [none]\n---\nUse $@ $@, \\$@ and $ARGUMENTS.',
    "command",
    "---\ndescription: Keep $@ here\nagent: architect\nmodel: x/y\nsubtask: true\n---\nUse $ARGUMENTS $ARGUMENTS, \\$ARGUMENTS and $ARGUMENTS.",
  );
});
test("absent and empty modes differ; last duplicate fields win including empty strings", () => {
  for (const body of [
    "---\nmode:\n---\nBody",
    "---\nmode: primary\nmode: \n---\nBody",
    "---\nmode: null\n---\nBody",
    "---\ndescription: first\nmodel: old\ndescription: last\nmodel: new\n---\nBody",
    '---\nDescription: ignored\nmodel: "quoted"\n---\nBody',
  ])
    text(body, "agent");
  text("---\nmode:\n---\nBody", "agent", "---\n---\nBody");
  text("Body", "agent", "---\nmode: subagent\n---\nBody");
});
test("frontmatter requires exact opening and first closing anchors, including the empty-header edge", () => {
  for (const value of [
    "",
    "Body\n",
    "\n---\ndescription: x\n---\nBody",
    "---",
    "---\ndescription: x\n---",
    "---\n---\nBody",
    "---\n\n---\nBody",
    "---\ndescription: first\n---\n---\ndescription: second\n---\nBody",
    "\ufeff---\ndescription: x\n---\nBody",
  ])
    for (const kind of ["agent", "command"]) text(value, kind);
});
test("reads normalize CRLF and lone CR before frontmatter recognition and preserve final-newline choices", () => {
  for (const value of [
    "---\r\ndescription: x\r\n---\r\n\r\nBody\r\n",
    "---\rdescription: x\r---\rBody",
    "---\ndescription: x\n---\n\n\nBody",
    "\n\nBody",
    "\n\n",
    "Body\n\n",
  ])
    for (const kind of ["agent", "command"]) text(value, kind);
});
test("frontmatter splitlines covers Python line separators, but not every whitespace code point", () => {
  for (const cp of [10, 11, 12, 13, 28, 29, 30, 0x85, 0x2028, 0x2029]) {
    const c = String.fromCodePoint(cp);
    text(
      "---\ndescription: first" + c + "model: x/y\n---\nBody",
      "agent",
      "---\ndescription: first\nmode: subagent\nmodel: x/y\n---\nBody",
    );
  }
  text(
    "---\ndescription: first\x1fmodel: x/y\n---\nBody",
    "agent",
    "---\ndescription: first\x1fmodel: x/y\nmode: subagent\n---\nBody",
  );
});
test("only ASCII-space-indented fields are skipped; keys and values use exact Unicode stripping", () => {
  text(
    "---\n description: ignored\n\tdescription\t: \u2003kept\u0085\n\u2003model: x/y\nmode :  primary\n---\nBody",
    "agent",
    "---\ndescription: kept\nmode: primary\nmodel: x/y\n---\nBody",
  );
  const whitespace = [
    9,
    11,
    12,
    28,
    29,
    30,
    31,
    32,
    0x85,
    0xa0,
    0x1680,
    ...Array.from({ length: 11 }, (_, i) => 0x2000 + i),
    0x2028,
    0x2029,
    0x202f,
    0x205f,
    0x3000,
  ];
  for (const cp of whitespace)
    text(
      "---\nskills: [one]\n---\nBody" + String.fromCodePoint(cp),
      "agent",
      "---\nmode: subagent\n---\nBody\n\n" + note + "one.\n",
    );
});
test("skills use character-set stripping and comma splitting, not YAML parsing", () => {
  for (const skills of [
    "[]",
    "[ ]",
    "[[[a]]]",
    "[a, b,, a]",
    '"[a,b]"',
    "[ , , ]",
    "[\u2003]",
    "[one\u0000two]",
    "[,first,,last,]",
  ])
    text("---\nskills: " + skills + "\n---\nBody \n", "agent");
  text(
    "---\nskills: [ , , ]\n---\nBody",
    "agent",
    "---\nmode: subagent\n---\nBody\n\n" + note + ".\n",
  );
});
test("skill notes are deduplicated by exact trimmed substring anywhere in the body", () => {
  const sentence = note + "one, two.";
  for (const body of [
    sentence,
    "Prefix " + sentence + " suffix\n",
    sentence + "\n\n",
    "Different case: " + sentence.toLowerCase(),
  ])
    text("---\nskills: [one, two]\n---\n" + body, "agent");
  text(
    "---\nskills: [one, two]\n---\nPrefix " + sentence + " suffix \n",
    "agent",
    "---\nmode: subagent\n---\nPrefix " + sentence + " suffix \n",
  );
});
test("valid embedded NUL and astral characters remain data in keys, values, notes and bodies", () => {
  for (const kind of ["agent", "command"])
    text(
      "---\ndescription: 尾🙂\0value\nmodel\0: ignored\nskills: [first\0skill, last]\n---\nBefore\0$@ after🙂\n",
      kind,
    );
  text(
    "---\nskills: [a]\n---\n\0" + note + "a.\0",
    "agent",
    "---\nmode: subagent\n---\n\0" + note + "a.\0",
  );
});
test("invalid UTF-8 anywhere in a source fails before truncating that destination", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const bytes of [
      Buffer.from([255]),
      Buffer.from([0, 255]),
      Buffer.from([0xed, 0xa0, 0x80]),
    ])
      temp((root) => {
        const src = join(root, "src"),
          dst = join(root, "dst");
        mkdirSync(src);
        mkdirSync(dst);
        writeFileSync(join(src, "x.md"), bytes);
        writeFileSync(join(dst, "x.md"), "keep");
        const r = run(src, dst, "agent", old);
        assert.equal(r.status, 1);
        assert.equal(readFileSync(join(dst, "x.md"), "utf8"), "keep");
      });
});
test("shallow glob includes hidden .md names and excludes uppercase extensions and nested markdown", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src"),
        dst = join(root, "dst");
      mkdirSync(src);
      for (const name of [
        "a.md",
        ".hidden.md",
        ".md",
        "UPPER.MD",
        "readme.txt",
      ])
        writeFileSync(join(src, name), "Body");
      mkdirSync(join(src, "nested"));
      writeFileSync(join(src, "nested/deep.md"), "Ignore");
      assert.equal(run(src, dst, "command", old).status, 0);
      assert.deepEqual(readdirSync(dst).sort(), [".hidden.md", ".md", "a.md"]);
    });
});
test("missing, non-directory and unsearchable sources yield empty globs after destination creation", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const type of [
      "missing",
      "file",
      ...(process.getuid?.() === 0 ? [] : ["unreadable"]),
    ])
      temp((root) => {
        const src = join(root, "src"),
          dst = join(root, "new/sub");
        if (type === "file") writeFileSync(src, "not a directory");
        if (type === "unreadable") {
          mkdirSync(src);
          writeFileSync(join(src, "a.md"), "hidden");
          chmodSync(src, 0);
        }
        try {
          assert.equal(run(src, dst, "agent", old).status, 0);
          assert.deepEqual(readdirSync(dst), []);
        } finally {
          if (type === "unreadable") chmodSync(src, 0o700);
        }
      });
});
test("file ordering preserves completed outputs when a later directory or dangling link fails", () => {
  for (const old of legacy ? [false, true] : [false])
    for (const type of ["directory", "dangling"])
      temp((root) => {
        const src = join(root, "src"),
          dst = join(root, "dst");
        mkdirSync(src);
        writeFileSync(join(src, "z.md"), "last");
        writeFileSync(join(src, "a.md"), "first");
        if (type === "directory") mkdirSync(join(src, "m.md"));
        else symlinkSync("absent", join(src, "m.md"));
        assert.equal(run(src, dst, "command", old).status, 1);
        assert.deepEqual(readdirSync(dst), ["a.md"]);
        assert.equal(
          readFileSync(join(dst, "a.md"), "utf8"),
          "---\n---\nfirst",
        );
      });
});
test("filename ordering decodes valid UTF-8 and uses surrogateescape for raw bytes", () => {
  let expected: any;
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src"),
        dst = join(root, "dst");
      mkdirSync(src);
      writeFileSync(
        Buffer.concat([
          Buffer.from(src + "/"),
          Buffer.from([255]),
          Buffer.from(".md"),
        ]),
        "raw first",
      );
      mkdirSync(join(src, "\ue000.md"));
      writeFileSync(join(src, "🙂.md"), "must stay absent");
      assert.equal(run(src, dst, "agent", old).status, 1);
      const files = tree(dst);
      assert.equal(files.length, 1);
      assert.equal(files[0][0], "ff2e6d64");
      if (old) assert.deepEqual(files, expected);
      else expected = files;
    });
});
test("source and destination links are followed; direct writes preserve inode and mode", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src"),
        dst = join(root, "dst");
      mkdirSync(src);
      mkdirSync(dst);
      writeFileSync(join(root, "input"), "Body");
      symlinkSync("../input", join(src, "a.md"));
      writeFileSync(join(root, "output"), "old");
      chmodSync(join(root, "output"), 0o640);
      const ino = statSync(join(root, "output")).ino;
      symlinkSync("../output", join(dst, "a.md"));
      symlinkSync("src", join(root, "source-link"));
      symlinkSync("dst", join(root, "destination-link"));
      assert.equal(
        run(
          join(root, "source-link"),
          join(root, "destination-link"),
          "command",
          old,
        ).status,
        0,
      );
      assert.equal(lstatSync(join(dst, "a.md")).isSymbolicLink(), true);
      assert.equal(statSync(join(root, "output")).ino, ino);
      assert.equal(statSync(join(root, "output")).mode & 0o777, 0o640);
      assert.equal(
        readFileSync(join(root, "output"), "utf8"),
        "---\n---\nBody",
      );
    });
});
test("repeated source overlays and in-place conversions preserve current mutation boundaries", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src"),
        other = join(root, "other"),
        dst = join(root, "dst");
      mkdirSync(src);
      mkdirSync(other);
      writeFileSync(join(src, "same.md"), "first");
      writeFileSync(join(src, "unique.md"), "unique");
      writeFileSync(join(other, "same.md"), "second $@");
      assert.equal(run(src, dst, "command", old).status, 0);
      assert.equal(run(other, dst, "command", old).status, 0);
      assert.equal(
        readFileSync(join(dst, "same.md"), "utf8"),
        "---\n---\nsecond $ARGUMENTS",
      );
      assert.equal(
        readFileSync(join(dst, "unique.md"), "utf8"),
        "---\n---\nunique",
      );
      assert.equal(run(src, src, "command", old).status, 0);
      assert.equal(
        readFileSync(join(src, "same.md"), "utf8"),
        "---\n---\nfirst",
      );
    });
});
test("destination creation, lexical dot/dot-dot handling and regular-file conflicts preserve Pathlib behavior", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src");
      mkdirSync(src);
      writeFileSync(join(src, "a.md"), "Body");
      assert.equal(
        run(src, root + "/missing/../dst//./", "agent", old).status,
        0,
      );
      assert.ok(existsSync(join(root, "missing")));
      writeFileSync(join(root, "file"), "keep");
      assert.equal(run(src, join(root, "file"), "agent", old).status, 1);
      assert.equal(readFileSync(join(root, "file"), "utf8"), "keep");
    });
});
test("failed open/write/close stops the tree without undoing earlier files", () => {
  for (const mode of ["open", "write", "close"])
    temp((root) => {
      const src = join(root, "src"),
        dst = join(root, "dst");
      mkdirSync(src);
      mkdirSync(dst);
      for (const name of ["a.md", "b.md", "c.md"])
        writeFileSync(join(src, name), "Body");
      writeFileSync(join(dst, "b.md"), "old");
      const result = run(src, dst, "command", false, {
        env: fault(join(dst, "b.md"), mode),
      });
      assert.equal(result.status, 1, result.stderr);
      assert.equal(readFileSync(join(dst, "a.md"), "utf8"), "---\n---\nBody");
      assert.equal(existsSync(join(dst, "c.md")), false);
      assert.equal(
        readFileSync(join(dst, "b.md"), "utf8"),
        mode === "open" ? "old" : mode === "write" ? "---" : "---\n---\nBody",
      );
    });
});
test("failed directory enumeration discards partial names rather than publishing a partial catalogue", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src"),
        dst = join(root, "dst");
      mkdirSync(src);
      for (const name of ["a.md", "b.md", "c.md"])
        writeFileSync(join(src, name), "Body");
      assert.equal(
        run(src, dst, "agent", old, { env: fault(src, "scan") }).status,
        0,
      );
      assert.deepEqual(readdirSync(dst), []);
    });
});
test("CLI choices, required kind, abbreviations, equals forms and help retain argparse semantics", () => {
  for (const old of legacy ? [false, true] : [false])
    temp((root) => {
      const src = join(root, "src");
      mkdirSync(src);
      function cli(args: string[]) {
        const r = spawnSync(
          old ? python : bin,
          old ? [legacy!, ...args] : args,
          { encoding: "utf8", cwd: root },
        );
        assert.ifError(r.error);
        return r;
      }
      for (const args of [
        [],
        ["src", "dst"],
        ["--kind", "bad", "src", "dst"],
        ["--kind=agent", "src"],
        ["--kind=agent", "src", "dst", "extra"],
        ["--kind", "--help"],
      ])
        assert.equal(cli(args).status, 2);
      for (const args of [
        ["-h"],
        ["--help"],
        ["--h"],
        ["-hh"],
        ["--unknown", "-h"],
      ])
        assert.equal(cli(args).status, 0);
      for (const args of [
        ["--k=agent", "src", "dst"],
        ["src", "--kind", "command", "dst"],
        ["--kind", "agent", "--kind", "command", "src", "dst"],
      ])
        assert.equal(cli(args).status, 0);
      assert.equal(
        cli(["--kind", "bad", "--kind", "agent", "src", "dst"]).status,
        2,
      );
    });
});
test(
  "all repository agents and local/upstream prompts retain byte-identical generated bundles",
  {
    skip:
      (!legacy && !process.env.OPENCODE_ADAPT_REFERENCE_BUNDLES) ||
      !process.env.OPENCODE_ADAPT_PI_ROOT ||
      !process.env.OPENCODE_ADAPT_UPSTREAM,
  },
  () => {
    const pi = process.env.OPENCODE_ADAPT_PI_ROOT!,
      upstream = process.env.OPENCODE_ADAPT_UPSTREAM!;
    for (const kind of ["agent", "command"])
      temp((root) => {
        const sources =
          kind === "agent"
            ? [join(pi, "agents")]
            : [join(pi, "prompts"), upstream];
        for (const source of sources) {
          assert.equal(run(source, join(root, "native"), kind).status, 0);
          if (legacy)
            assert.equal(
              run(source, join(root, "legacy"), kind, true).status,
              0,
            );
        }
        const reference = legacy
          ? join(root, "legacy")
          : JSON.parse(
              readFileSync(
                process.env.OPENCODE_ADAPT_REFERENCE_BUNDLES!,
                "utf8",
              ),
            )[kind === "agent" ? "agents" : "commands"];
        assert.deepEqual(tree(join(root, "native")), tree(reference));
        assert.ok(readdirSync(join(root, "native")).length > 0);
      });
  },
);
