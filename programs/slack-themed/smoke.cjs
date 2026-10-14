"use strict";
// Offline native-renderer check. Run via smoke.sh, never against a real profile.
const { app, BrowserWindow, protocol, session } = require("electron");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { once } = require("node:events");
const theme = require("./dotfiles-theme.cjs");
const state = process.env.XDG_STATE_HOME;
const output = process.env.SLACK_THEME_SMOKE_OUTPUT;
assert(state && output && state.startsWith(output + "/"));
app.setPath("userData", path.join(output, "profile"));
const file = path.join(state, "theme-menu/active/hyprland.conf");
fs.mkdirSync(path.dirname(file), { recursive: true });
const original = fs.readFileSync(process.env.SLACK_THEME_SMOKE_SOURCE, "utf8");
fs.writeFileSync(file, original);
assert(theme.allowedURL("https://app.slack.com/client/test"));
assert(theme.allowedURL("https://example.slack.com/client/test"));
assert(!theme.allowedURL("https://slack.com.evil.example/"));
assert(!theme.allowedURL("https://evilslack.com/"));
assert(!theme.allowedURL("file:///etc/passwd"));
assert(!theme.allowedURL("http://app.slack.com/"));
assert.equal(theme.selectionText({ selection: "#393936", accent: "#005599", text: "#202020", background: "#fafafa" }), "#fafafa");
assert.throws(() => theme.parseTheme(original.replace(/\$theme_font = .+/, '$theme_font = x"; } body {')));

protocol.registerSchemesAsPrivileged([{ scheme: "app", privileges: { standard: true, secure: true } }]);
theme.install();
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const fixture = `<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Slack theme offline preview</title><style>
* { box-sizing: border-box; } body { margin: 0; font: 16px sans-serif; background: white; }
.p-top_nav { padding: 14px 24px; border-bottom: 1px solid; }
.p-workspace { display: grid; grid-template-columns: 220px 1fr; min-height: 600px; }
.p-channel_sidebar { padding: 24px 14px; } .p-channel_sidebar__channel { padding: 10px; border-radius: 6px; }
main { padding: 24px; } .p-view_header { padding: 12px; margin: -24px -24px 24px; }
.c-message_kit__background { padding: 16px 0; } .c-timestamp { font-size: 13px; }
.c-texty_input { padding: 14px; margin-top: 28px; border: 1px solid; border-radius: 8px; }
textarea { width: 100%; min-height: 80px; resize: vertical; border: 0; }
button { margin-top: 10px; padding: 8px 16px; } pre { padding: 12px; }
</style></head><body>
<header class="p-top_nav">Offline theme preview · synthetic content, no workspace connection</header>
<div class="p-workspace"><aside class="p-channel_sidebar"><h3>Example workspace</h3>
<div class="p-channel_sidebar__channel p-channel_sidebar__channel--selected"><a class="p-channel_sidebar__link" href="#general"># general</a></div>
<div class="p-channel_sidebar__channel"># development</div><div class="p-channel_sidebar__channel"># design</div></aside>
<main><header class="p-view_header"><strong># general</strong></header>
<article class="c-message_kit__background"><strong class="c-message_kit__sender">Example person</strong> <span class="c-timestamp">10:30</span>
<p>This local preview uses the active desktop palette and fonts.</p><a href="#details">Review theme details</a>
<pre><code>Theme changes apply without restarting Slack.</code></pre></article>
<div class="c-texty_input"><textarea class="ql-editor" aria-label="Message preview" placeholder="Type here to inspect focus. Nothing is sent."></textarea>
<button type="button">Preview button</button></div></main></div></body></html>`;

(async () => {
  await app.whenReady();
  // No network, authentication, real Slack data or remote renderer code.
  session.defaultSession.webRequest.onBeforeRequest((details, callback) => {
    callback({ cancel: !["app:", "data:"].includes(new URL(details.url).protocol) });
  });
  protocol.handle("app", () => new Response(fixture, { headers: { "content-type": "text/html" } }));
  const win = new BrowserWindow({ width: 1100, height: 750, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await win.loadURL("app://theme-preview");
  await sleep(300);
  const styles = () => win.webContents.executeJavaScript(`({
    background: getComputedStyle(document.body).backgroundColor,
    foreground: getComputedStyle(document.body).color,
    font: getComputedStyle(document.body).fontFamily,
    sidebar: getComputedStyle(document.querySelector('.p-channel_sidebar')).backgroundColor
  })`);
  const rgb = (hex) => `rgb(${[1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)).join(", ")})`;
  const active = theme.parseTheme(original);
  let observed = await styles();
  assert.equal(observed.background, rgb(active.background));
  assert.equal(observed.foreground, rgb(active.text));
  assert.equal(observed.sidebar, rgb(active.surface));
  assert(observed.font.includes(active.font));
  await win.webContents.executeJavaScript("document.querySelector('textarea').focus()");
  assert.equal(await win.webContents.executeJavaScript("document.activeElement.tagName"), "TEXTAREA");
  fs.writeFileSync(path.join(output, "active.png"), (await win.webContents.capturePage()).toPNG());

  const light = original.replace(/\$theme_background = .+/, "$theme_background = rgb(fafafa)")
    .replace(/\$theme_surface = .+/, "$theme_surface = rgb(e8e8e8)")
    .replace(/\$theme_text = .+/, "$theme_text = rgb(202020)")
    .replace(/\$theme_accent = .+/, "$theme_accent = rgb(005599)")
    .replace(/\$theme_muted = .+/, "$theme_muted = rgb(555555)")
    .replace(/\$theme_mode = .+/, "$theme_mode = light");
  fs.writeFileSync(file + ".new", light);
  fs.renameSync(file + ".new", file);
  await sleep(2300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  assert.equal(await win.webContents.executeJavaScript("getComputedStyle(document.querySelector('.p-channel_sidebar__link')).color"), rgb(theme.selectionText(theme.parseTheme(light))));
  fs.writeFileSync(path.join(output, "light.png"), (await win.webContents.capturePage()).toPNG());
  const loaded = once(win.webContents, "did-finish-load");
  win.reload();
  await loaded;
  await sleep(300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  fs.writeFileSync(file, "invalid theme");
  await sleep(2300);
  assert.equal((await styles()).background, "rgb(250, 250, 250)");
  fs.writeFileSync(file, original);
  await sleep(2300);
  assert.equal((await styles()).background, rgb(active.background));
  await win.loadURL("data:text/html,<body>Out of scope</body>");
  await sleep(2300);
  assert.notEqual(await win.webContents.executeJavaScript("getComputedStyle(document.body).backgroundColor"), rgb(active.background));
  console.log("PASS: origin boundary, CSS input validation, active palette/fonts, focus, live switch, reload, invalid-theme recovery, unthemed navigation");
  app.exit(0);
})().catch((error) => { console.error(error); app.exit(1); });
