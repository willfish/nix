"use strict";
const fs = require("node:fs");
const path = require("node:path");
const [root, theme] = process.argv.slice(2);
const manifest = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
// Fail the build rather than silently patching an unfamiliar upstream layout.
if (manifest.main !== "bundle.js") {
  throw new Error("Discord entry point changed; review the theme integration");
}
const boot = path.join(root, manifest.main);
const source = fs.readFileSync(boot, "utf8");
if (source.includes("dotfiles-theme.cjs")) throw new Error("Discord is already patched");
fs.copyFileSync(theme, path.join(root, "dotfiles-theme.cjs"));
fs.writeFileSync(boot, 'require("./dotfiles-theme.cjs").install();\n' + source);
