"use strict";
// Synthetic documents in Discord's shipped Electron runtime, never a real profile.
const { app, BrowserWindow, protocol, session } = require("electron");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { once } = require("node:events");
const theme = require("./dotfiles-theme.cjs");
const output = process.env.DISCORD_THEME_SMOKE_OUTPUT;
const state = process.env.XDG_STATE_HOME;
assert(output && state && state.startsWith(output + "/"));
app.setPath("userData", path.join(output, "profile"));
const file = path.join(state, "theme-menu/active/hyprland.conf");
fs.mkdirSync(path.dirname(file), { recursive: true });
const original = fs.readFileSync(process.env.DISCORD_THEME_SMOKE_SOURCE, "utf8");
assert(theme.allowedURL("https://discord.com/channels/example"));
assert(theme.allowedURL("https://discordapp.com/app"));
for (const url of ["https://discord.com.evil.example/", "https://evildiscord.com/", "https://cdn.discordapp.com/", "https://discord.com:8443/", "file:///etc/passwd", "http://discord.com/", "data:text/html,example"]) {
  assert(!theme.allowedURL(url), url);
}
assert.throws(() => theme.parseTheme(original.replace(/\$theme_font = .+/, '$theme_font = x"; } body {')));
assert.throws(() => theme.parseTheme(original.replace(/\$theme_background = .+/, '$theme_background = url(https://example.invalid)')));
const rgb = (hex) => `rgb(${[1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)).join(", ")})`;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const fixture = `<!doctype html><html class="theme-dark"><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Discord offline theme preview</title><style>
:root, .theme-dark, .theme-light { --background-base-low: white !important; --text-default: black !important;
 --background-base-lower: #ddd; --text-muted: #333; --font-primary: sans-serif; --font-code: monospace;
 --interactive-background-selected: #bbb; --interactive-text-active: black; --border-subtle: #999;
 --input-background-default: white; --input-text-default: black; --input-placeholder-text-default: #333;
 --control-primary-background-default: #5865f2; --control-primary-text-default: white;
 --text-link: #5865f2; --__adaptive-focus-ring-color: blue; }
* { box-sizing: border-box; } body { margin: 0; font: 16px var(--font-primary); color: var(--text-default); background: var(--background-base-low); }
header { padding: 16px 24px; background: var(--background-base-lower); border-bottom: 1px solid var(--border-subtle); }
.workspace { display: grid; grid-template-columns: 220px 1fr; min-height: 620px; }
aside { padding: 24px 14px; background: var(--background-base-lower); }
.channel { padding: 12px; border-radius: 6px; color: var(--text-muted); }
.selected { background: var(--interactive-background-selected); color: var(--interactive-text-active); }
main { padding: 28px; min-width: 0; } .timestamp { color: var(--text-muted); font-size: 13px; }
a { color: var(--text-link); } pre { padding: 16px; font-family: var(--font-code); background: var(--background-base-lower); white-space: pre-wrap; overflow-wrap: anywhere; }
textarea { width: 100%; min-height: 90px; padding: 14px; font: inherit; border: 1px solid var(--border-subtle);
 background: var(--input-background-default); color: var(--input-text-default); border-radius: 8px; }
textarea::placeholder { color: var(--input-placeholder-text-default); }
button { margin-top: 12px; padding: 10px 20px; border: 0; border-radius: 5px; font: inherit;
 background: var(--control-primary-background-default); color: var(--control-primary-text-default); }
:focus-visible { outline: 2px solid var(--__adaptive-focus-ring-color); outline-offset: 2px; }
</style></head><body><header>Offline Discord theme preview · synthetic content, no account connection</header>
<div class="workspace"><aside><h3>Example server</h3><div class="channel selected"># general</div>
<div class="channel"># development</div><div class="channel"># design</div></aside>
<main><h2># general</h2><p><strong>Example person</strong> <span class="timestamp">10:30</span></p>
<p>Colours and fonts follow the selected desktop palette. Layout and content stay unchanged.</p>
<p><a href="#details">Example link</a></p><pre>theme-menu changes the palette without restarting Discord.</pre>
<textarea aria-label="Message preview" placeholder="Type to inspect focus. Nothing is sent."></textarea>
<button type="button">Preview button</button></main></div></body></html>`;

(async () => {
  await app.whenReady();
  // Intercept HTTPS locally and block all non-fixture requests before networking.
  const urls = ["https://discord.com/channels/theme-preview", "https://discordapp.com/app"];
  session.defaultSession.webRequest.onBeforeRequest((details, callback) => {
    callback({ cancel: !urls.includes(details.url) && !details.url.startsWith("data:") });
  });
  protocol.handle("https", () => new Response(fixture, { headers: { "content-type": "text/html" } }));
  const win = new BrowserWindow({ width: 1100, height: 760, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await win.loadURL(urls[0]);
  const styles = () => win.webContents.executeJavaScript(`({
    background: getComputedStyle(document.body).backgroundColor,
    foreground: getComputedStyle(document.body).color,
    font: getComputedStyle(document.body).fontFamily,
    sidebar: getComputedStyle(document.querySelector('aside')).backgroundColor,
    muted: getComputedStyle(document.querySelector('.timestamp')).color,
    mode: getComputedStyle(document.documentElement).colorScheme,
    accent: getComputedStyle(document.querySelector('button')).backgroundColor,
    onAccent: getComputedStyle(document.querySelector('button')).color,
    selected: getComputedStyle(document.querySelector('.selected')).color
  })`);
  await sleep(300);
  assert.equal((await styles()).background, "rgb(255, 255, 255)");
  fs.writeFileSync(file + ".new", original);
  fs.renameSync(file + ".new", file);
  await sleep(2300);
  if (process.env.DISCORD_DOTFILES_THEME === "0") {
    assert.equal((await styles()).background, "rgb(255, 255, 255)");
    console.log("PASS: launch-time disable leaves the renderer unthemed");
    app.exit(0);
    return;
  }
  const active = theme.parseTheme(original);
  let observed = await styles();
  assert.equal(observed.background, rgb(active.background));
  assert.equal(observed.foreground, rgb(active.text));
  assert.equal(observed.sidebar, rgb(active.surface));
  assert.equal(observed.mode, active.mode);
  assert(observed.font.includes(active.font));
  const hex = (value) => "#" + value.match(/\d+/g).map((n) => Number(n).toString(16).padStart(2, "0")).join("");
  assert(theme.contrast(hex(observed.muted), active.background) >= 4.5);
  assert(theme.contrast(hex(observed.muted), active.surface) >= 4.5);
  assert(theme.contrast(hex(observed.onAccent), active.accent) >= 4.5);
  win.focus();
  win.webContents.focus();
  await win.webContents.executeJavaScript("document.querySelector('a').focus()");
  win.webContents.sendInputEvent({ type: "keyDown", keyCode: "Tab" });
  win.webContents.sendInputEvent({ type: "keyUp", keyCode: "Tab" });
  await sleep(100);
  assert.equal(await win.webContents.executeJavaScript("document.activeElement.tagName"), "TEXTAREA");
  assert.equal(await win.webContents.executeJavaScript("getComputedStyle(document.activeElement).outlineColor"), rgb(active.accent));
  fs.writeFileSync(path.join(output, "active.png"), (await win.webContents.capturePage()).toPNG());

  const light = original.replace(/\$theme_background = .+/, "$theme_background = rgb(fafafa)")
    .replace(/\$theme_surface = .+/, "$theme_surface = rgb(e8e8e8)")
    .replace(/\$theme_selection = .+/, "$theme_selection = rgb(d0d8e0)")
    .replace(/\$theme_text = .+/, "$theme_text = rgb(202020)")
    .replace(/\$theme_accent = .+/, "$theme_accent = rgb(005599)")
    .replace(/\$theme_muted = .+/, "$theme_muted = rgb(555555)")
    .replace(/\$theme_mode = .+/, "$theme_mode = light");
  fs.writeFileSync(file + ".new", light);
  fs.renameSync(file + ".new", file);
  await win.webContents.executeJavaScript("document.documentElement.className = 'theme-light'");
  await sleep(2300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  assert.equal((await styles()).mode, "light");
  assert.equal((await styles()).accent, "rgb(0, 85, 153)");
  fs.writeFileSync(path.join(output, "light.png"), (await win.webContents.capturePage()).toPNG());
  const loaded = once(win.webContents, "did-finish-load");
  win.reload();
  await loaded;
  await sleep(300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  const popout = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await popout.loadURL(urls[1]);
  await sleep(300);
  assert.equal(await popout.webContents.executeJavaScript("getComputedStyle(document.body).backgroundColor"), "rgb(250, 250, 250)");
  popout.destroy();
  fs.writeFileSync(file, "invalid theme");
  await sleep(2300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  fs.writeFileSync(file + ".new", original);
  fs.renameSync(file + ".new", file);
  await sleep(2300);
  assert.equal((await styles()).background, rgb(active.background));
  await win.loadURL("data:text/html,<body>Out of scope</body>");
  await sleep(300);
  assert.notEqual(await win.webContents.executeJavaScript("getComputedStyle(document.body).backgroundColor"), rgb(active.background));
  console.log("PASS: packaged startup hook, missing-state startup, origin and input boundaries, palette/fonts, contrast, focus, atomic live switch, light mode, reload, new window, invalid-state recovery, unthemed navigation");
  app.exit(0);
})().catch((error) => { console.error(error); app.exit(1); });
