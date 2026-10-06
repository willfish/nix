import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  chmodSync,
  writeFileSync,
  readFileSync,
  copyFileSync,
  rmSync,
  statSync,
  symlinkSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
const binary = process.env.COMMUNITY_IMPORT_BIN!,
  legacy = process.env.COMMUNITY_IMPORT_LEGACY,
  python = process.env.COMMUNITY_IMPORT_PYTHON || "python3";
assert.ok(binary, "Set COMMUNITY_IMPORT_BIN");
const start = "    # BEGIN generated Omarchy community inputs",
  end = "    # END generated Omarchy community inputs";
const initial = "before\n" + start + "\nold block\n" + end + "\nafter\n";
const source = "https://example.test/pinned/commit/themes.html";
function figure(
  slug = "blue",
  label = "Blue",
  url = "https://github.com/owner/blue",
) {
  return `<figure><img src="/themes/${slug}.png"><figcaption><a href="${url}">${label}</a></figcaption></figure>`;
}
function check(
  page: string,
  status = 0,
  unavailable: any = {},
  flake = initial,
  verify?: (root: string) => void,
) {
  let expected: any;
  for (const old of legacy ? [false, true] : [false]) {
    const root = mkdtempSync(join(tmpdir(), "community-import-"));
    try {
      mkdirSync(join(root, "home/user/themes"), { recursive: true });
      mkdirSync(join(root, "scripts"));
      writeFileSync(join(root, "flake.nix"), flake);
      writeFileSync(
        join(root, "home/user/themes/community-unavailable.json"),
        JSON.stringify(unavailable),
      );
      writeFileSync(join(root, "home/user/themes/community.json"), "previous");
      chmodSync(join(root, "flake.nix"), 0o640);
      chmodSync(join(root, "home/user/themes/community.json"), 0o600);
      const inode = statSync(join(root, "flake.nix")).ino;
      writeFileSync(join(root, "page.html"), page);
      if (old)
        copyFileSync(
          legacy!,
          join(root, "scripts/import-omarchy-community.py"),
        );
      const r = spawnSync(
        old ? python : binary,
        old
          ? [
              join(root, "scripts/import-omarchy-community.py"),
              join(root, "page.html"),
              source,
            ]
          : ["--root", root, join(root, "page.html"), source],
        { encoding: "utf8", timeout: 10000, cwd: tmpdir() },
      );
      assert.ifError(r.error);
      assert.equal(r.status, status, r.stderr);
      assert.doesNotMatch(
        r.stderr,
        /AddressSanitizer|LeakSanitizer|runtime error:|DEADLYSIGNAL/,
      );
      if (status) {
        assert.equal(readFileSync(join(root, "flake.nix"), "utf8"), flake);
        assert.equal(
          readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
          "previous",
        );
      } else {
        assert.equal(statSync(join(root, "flake.nix")).ino, inode);
        assert.equal(statSync(join(root, "flake.nix")).mode & 0o777, 0o640);
        assert.equal(
          statSync(join(root, "home/user/themes/community.json")).mode & 0o777,
          0o600,
        );
        const result = {
          flake: readFileSync(join(root, "flake.nix"), "utf8"),
          catalogue: JSON.parse(
            readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
          ),
          stdout: r.stdout,
        };
        if (!old) expected = result;
        else assert.deepEqual(result, expected);
        verify?.(root);
      }
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  }
  return expected;
}

test("valid figures produce sorted inputs and a full Unicode catalogue, filtering unavailable repositories only from the flake", () => {
  const result = check(
    figure("zebra", "Zèbre &amp; 雪", "https://github.com/vendor/zebra/") +
      figure("blue", "Blue"),
    0,
    { zebra: "private or deleted" },
  );
  assert.equal(
    result.flake,
    "before\n" +
      start +
      '\n    omarchy-theme-blue = {\n      url = "git+https://github.com/owner/blue?shallow=1";\n      flake = false;\n    };\n' +
      end +
      "\nafter\n",
  );
  assert.deepEqual(Object.keys(result.catalogue.themes), ["blue", "zebra"]);
  assert.equal(result.catalogue.themes.zebra.label, "Zèbre & 雪");
  assert.equal(result.catalogue.source, source);
  assert.match(result.stdout, /Read 2 community themes/);
  assert.match(result.stdout, /Unavailable: zebra: private or deleted/);
});
test("HTML token parsing preserves nested caption text, entities, comments, mixed case and duplicate-attribute last wins", () => {
  check(
    `<!doctype html><html><!-- ignored ' quote --><body><FIGURE><IMG SRC='discard.png' src='/images/blue.png'><FIGCAPTION> Before <A href='https://bad.test/no' HREF='https://github.com/owner/blue'>Blue &NoBreak; &amp; <b>bold</b></A> after </FIGCAPTION></FIGURE></body></html>`,
    0,
    {},
    initial,
    (root) => {
      const entry = JSON.parse(
        readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
      ).themes.blue;
      assert.equal(entry.label, "Before Blue \u2060 & bold after");
    },
  );
});
test("last image and caption link win, while links outside captions are ignored", () => {
  check(
    `<figure><a href='https://bad.test/ignore'>Outside</a><img src='first.png'><img src='last.webp'><figcaption><a href='https://github.com/owner/first'>First</a><a href='https://github.com/owner/last///'>Last</a></figcaption></figure>`,
    0,
    {},
    initial,
    (root) => {
      const entry = JSON.parse(
        readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
      ).themes.last;
      assert.equal(entry.url, "https://github.com/owner/last");
      assert.equal(entry.label, "FirstLast");
    },
  );
});
test("empty pages and incomplete or self-closing figures never synthesize catalogue entries", () => {
  for (const page of [
    "",
    "<html><p>none</p></html>",
    '<figure><img src="blue.png"><figcaption><a href="https://github.com/owner/blue">Blue</a></figcaption>',
    "<figure/>",
    "<figure/>" + figure(),
  ])
    check(page, 1);
  check(figure() + '<figure><img src="unclosed.png">', 0, {}, initial, (root) =>
    assert.deepEqual(
      Object.keys(
        JSON.parse(
          readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
        ).themes,
      ),
      ["blue"],
    ),
  );
});
test("invalid IDs, duplicates and unsupported URLs fail before either output changes", () => {
  for (const slug of [
    "blue_theme",
    "Blue",
    "blue--theme",
    "-blue",
    "blue-",
    "a".repeat(54),
    "blue.dot",
  ])
    check(figure(slug), 1);
  check(figure("../escape"), 0);
  check(figure() + figure(), 1);
  for (const url of [
    "http://github.com/owner/repo",
    "https://evil.test/owner/repo",
    "https://user@github.com/owner/repo",
    "https://github.com/owner/repo?x=1",
    "https://github.com/owner/repo/tree/main",
    "https://github.com/owner/repo#fragment",
  ])
    check(figure("blue", "Blue", url), 1);
});
test("labels strip Unicode end whitespace but reject empty and embedded ASCII controls", () => {
  check(
    figure("blue", "\u0085\u2003Blue &amp; White\u3000"),
    0,
    {},
    initial,
    (root) =>
      assert.equal(
        JSON.parse(
          readFileSync(join(root, "home/user/themes/community.json"), "utf8"),
        ).themes.blue.label,
        "Blue & White",
      ),
  );
  for (const label of ["", " \t\n ", "bad&#10;label", "bad&#9;label"])
    check(figure("blue", label), 1);
});
test("missing required figure data and duplicate unsafe final attributes are rejected", () => {
  for (const body of [
    '<figure><figcaption><a href="https://github.com/owner/blue">Blue</a></figcaption></figure>',
    '<figure><img src="blue.png"></figure>',
    '<figure><img src><figcaption><a href="https://github.com/owner/blue">Blue</a></figcaption></figure>',
    figure().replace(
      'href="https://github.com/owner/blue"',
      'href="https://github.com/owner/blue" href="javascript:bad"',
    ),
  ])
    check(body, 1);
});
test("generated markers must occur exactly once and in order", () => {
  for (const flake of [
    "none",
    start + "\nmissing",
    start + "\n" + start + "\n" + end,
    start + "\n" + end + "\n" + end,
    end + "\n" + start,
  ])
    check(figure(), 1, {}, flake);
});
test("all-unavailable catalogues retain website entries but emit an empty marked input block", () => {
  const result = check(figure(), 0, { blue: "unavailable" });
  assert.equal(result.flake, "before\n" + start + "\n" + end + "\nafter\n");
  assert.ok(result.catalogue.themes.blue);
});
test(
  "pinned website HTML matches the retained importer on a disposable checkout",
  { skip: !process.env.COMMUNITY_IMPORT_PAGE },
  () => {
    const unavailable = JSON.parse(
      readFileSync(
        new URL(
          "../../../home/user/themes/community-unavailable.json",
          import.meta.url,
        ),
        "utf8",
      ),
    );
    const result = check(
      readFileSync(process.env.COMMUNITY_IMPORT_PAGE!, "utf8"),
      0,
      unavailable,
    );
    assert.ok(Object.keys(result.catalogue.themes).length > 0);
    console.log(
      `Compared ${Object.keys(result.catalogue.themes).length} pinned community themes`,
    );
  },
);
test("direct writes retain existing file modes and outside-block text byte for byte", () => {
  check(
    figure(),
    0,
    {},
    "prefix\r\n" + start + "\r\nold\r\n" + end + "\r\nsuffix\r\n",
    (root) => {
      assert.equal(
        readFileSync(join(root, "flake.nix"), "utf8").endsWith("\nsuffix\n"),
        true,
      );
      assert.equal(statSync(join(root, "flake.nix")).isFile(), true);
    },
  );
});
