"use strict";
const fs = require("node:fs");
const path = require("node:path");
const [root, theme] = process.argv.slice(2);
const manifest = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
// Fail the build instead of silently patching an unfamiliar upstream layout.
if (manifest.main !== "./dist/boot.bundle.cjs") {
  throw new Error("Slack entry point changed; review the theme integration");
}
const boot = path.join(root, manifest.main);
const source = fs.readFileSync(boot, "utf8");
const hook = 'require("./dotfiles-theme.cjs").install();\n';
if (source.includes("dotfiles-theme.cjs")) throw new Error("Slack is already patched");
fs.copyFileSync(theme, path.join(root, "dist/dotfiles-theme.cjs"));
fs.writeFileSync(boot, hook + source);
