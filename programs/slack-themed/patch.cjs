"use strict";
const fs = require("node:fs");
const path = require("node:path");

function patch(root, themePath, keysPath) {
  const manifest = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8"));
  if (manifest.main !== "./dist/boot.bundle.cjs") {
    throw new Error("Slack entry point changed; review the theme integration");
  }
  const boot = path.join(root, manifest.main);
  const source = fs.readFileSync(boot, "utf8");
  if (source.includes("dotfiles-theme.cjs") || source.includes("dotfiles-keys.cjs")) {
    throw new Error("Slack is already patched");
  }
  fs.copyFileSync(themePath, path.join(root, "dist/dotfiles-theme.cjs"));
  fs.copyFileSync(keysPath, path.join(root, "dist/dotfiles-keys.cjs"));
  fs.writeFileSync(boot, 'require("./dotfiles-theme.cjs").install();\nrequire("./dotfiles-keys.cjs").install();\n' + source);
}

module.exports = { patch };

if (require.main === module) {
  const [root, theme, keys] = process.argv.slice(2);
  patch(root, theme, keys);
}
