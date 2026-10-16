"use strict";
// Offline native-renderer check. Run via smoke.sh, never against a real profile.
const { app, BrowserWindow, protocol, session } = require("electron");
const assert = require("node:assert/strict");
const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");
const { once } = require("node:events");
const theme = require("./dotfiles-theme.cjs");
const keys = require("./dotfiles-keys.cjs");
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
keys.install();
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const fixture = `<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Slack theme offline preview</title><style>
* { box-sizing: border-box; } body { margin: 0; font: 16px sans-serif; background: white; }
.p-top_nav { padding: 14px 24px; border-bottom: 1px solid; }
.p-workspace { display: grid; grid-template-columns: 200px minmax(0, 1fr) 240px; min-height: 560px; }
.p-channel_sidebar { padding: 24px 14px; } .p-channel_sidebar__channel { padding: 10px; border-radius: 6px; }
main { padding: 24px; } .p-view_header { padding: 12px; margin: -24px -24px 24px; }
.c-message_kit__background { padding: 16px 0; } .c-timestamp { font-size: 13px; }
.c-texty_input { padding: 14px; margin-top: 28px; border: 1px solid; border-radius: 8px; }
textarea { width: 100%; min-height: 80px; resize: vertical; border: 0; }
button { margin-top: 10px; padding: 8px 16px; } pre { padding: 12px; }
</style></head><body>
<header class="p-top_nav">Offline theme preview · synthetic content, no workspace connection <input class="p-top_nav__search" type="search" aria-label="Search"></header>
<div class="p-workspace"><aside class="p-channel_sidebar"><h3>Example workspace</h3>
<div class="p-channel_sidebar__channel p-channel_sidebar__channel--selected"><a class="p-channel_sidebar__link" href="#general"># general</a></div>
<div class="p-channel_sidebar__channel"># development</div><div class="p-channel_sidebar__channel"># design</div>
<div id="shadow-host" style="width:88px;height:32px;margin-top:8px"></div>
<div id="row" role="button" tabindex="0" style="width:160px;height:34px;margin-top:8px;position:relative"><button type="button" id="row-button" style="position:absolute;left:4px;top:2px;width:154px;height:31px">Inner</button></div>
<div id="pad-row" role="button" tabindex="0" style="width:180px;height:64px;margin-top:8px;position:relative"><button type="button" id="pad-button" style="position:absolute;left:4px;top:4px;width:48px;height:20px">Pad</button></div>
<div id="clip-box" style="width:20px;height:20px;overflow:hidden;position:relative;margin-top:8px"><button type="button" id="clipped-btn" style="position:absolute;left:40px;top:0;width:40px;height:20px">Clip</button></div>
</aside>
<main class="p-workspace__primary_view p-message_pane"><header class="p-view_header"><strong># general</strong></header>
<div class="c-virtual_list__scroll_container" id="scroller" tabindex="0" style="height:200px;overflow:auto;position:relative">
<div style="height:2000px"><div id="nested-scroll" tabindex="0" style="height:72px;width:160px;overflow:auto"><div style="height:360px;width:480px">nested content</div></div>
<a id="center-link" href="#center" style="position:absolute;left:35%;top:78px">Center link</a>
<article class="c-message_kit__background"><strong class="c-message_kit__sender">Example person</strong> <span class="c-timestamp">10:30</span>
<div class="c-texty_input" style="margin-top:0"><div class="ql-editor" id="inline-edit" contenteditable="true" role="textbox" aria-label="Edit message"><p style="margin:0">Earlier inline edit</p></div></div>
<p>This local preview uses the active desktop palette and fonts.</p><a href="#details">Review theme details</a>
<pre><code>Theme changes apply without restarting Slack.</code></pre></article></div></div>
<div class="p-message_input"><div class="c-texty_input"><textarea id="primary-composer" class="ql-editor" aria-label="Message preview" placeholder="Type here to inspect focus. Nothing is sent."></textarea>
<button type="button" id="preview-button">Preview button</button></div></div></main>
<aside class="p-workspace__secondary_view"><div id="thread-body" tabindex="0">Thread</div>
<article class="c-message_kit__background"><div class="c-texty_input" style="margin-top:0"><div class="ql-editor" id="thread-inline" contenteditable="true" role="textbox" aria-label="Edit thread"><p style="margin:0">Thread inline edit</p></div></div></article>
<button type="button" id="thread-hint">Thread hint</button>
<div class="p-message_input"><div class="c-texty_input"><div class="ql-editor" id="thread-composer" contenteditable="true" role="textbox" aria-label="Thread reply"></div></div></div></aside>
<div class="ql-editor" id="rich-editor" contenteditable="true" role="textbox" style="position:fixed;left:500px;top:480px;width:240px;height:80px;padding:0;z-index:6"><p style="margin:0;height:80px">Hello</p></div>
<div class="ql-editor" id="rich-nested" contenteditable="true" role="textbox" style="position:fixed;left:760px;top:480px;width:240px;height:80px;padding:0;z-index:6"><p style="margin:0;height:80px">Text</p><button type="button" id="editor-button" style="position:absolute;right:8px;top:8px;width:64px;height:24px">Act</button></div>
<button type="button" id="occluded-btn" style="position:fixed;left:980px;top:640px;width:36px;height:20px">Hide</button>
<div id="occluder" style="position:fixed;left:970px;top:630px;width:70px;height:40px;background:#fff"></div>
</div></body></html>`;

(async () => {
  await app.whenReady();
  app.on("web-contents-created", (_event, contents) => {
    contents.on("console-message", (event) => {
      const message = event && event.message;
      if (typeof message === "string" && message.includes("Uncaught")) console.error("renderer:", message);
    });
  });
  // No network, authentication, real Slack data or remote renderer code.
  session.defaultSession.webRequest.onBeforeRequest((details, callback) => {
    callback({ cancel: !["app:", "data:"].includes(new URL(details.url).protocol) });
  });
  protocol.handle("app", () => new Response(fixture, { headers: { "content-type": "text/html" } }));
  const win = new BrowserWindow({
    show: false,
    focusable: false,
    skipTaskbar: true,
    width: 1100,
    height: 750,
    title: "slack-theme-smoke",
    webPreferences: {
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      offscreen: true,
    },
  });
  let paints = 0;
  win.webContents.setFrameRate(30);
  win.webContents.on("paint", () => { paints += 1; });
  win.webContents.startPainting();
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
  const ready = async () => {
    for (let attempt = 0; attempt < 20; attempt += 1) {
      if (await win.webContents.executeJavaScript("window.__slackDotfilesKeys === true")) return;
      await sleep(50);
    }
    throw new Error("keymap was not injected");
  };
  await ready();
  const bridge = [];
  win.webContents.on("console-message", (event) => {
    const message = event && event.message;
    if (typeof message === "string" && message.startsWith("SLACK_DOTFILES_KEYS")) bridge.push(message);
  });
  const setMotion = async (value) => {
    if (!win.webContents.debugger.isAttached()) win.webContents.debugger.attach("1.3");
    await win.webContents.debugger.sendCommand("Emulation.setEmulatedMedia", {
      features: [{ name: "prefers-reduced-motion", value }],
    });
    const matches = await win.webContents.executeJavaScript(`matchMedia("(prefers-reduced-motion: reduce)").matches`);
    if (value === "reduce" && matches !== true) throw new Error("reduced motion was not emulated");
    if (value === "no-preference" && matches !== false) throw new Error("normal motion was not emulated");
    return matches;
  };
  await setMotion("reduce");
  await win.webContents.executeJavaScript(`(() => {
    window.clicked = false;
    window.keysSeen = [];
    window.clickLog = [];
    window.wheelEvents = [];
    window.jKeyups = 0;
    document.querySelector("#preview-button").addEventListener("click", () => { window.clicked = true; });
    const host = document.querySelector("#shadow-host");
    const root = host.attachShadow({ mode: "open" });
    const shadowBtn = document.createElement("button");
    shadowBtn.id = "shadow-btn";
    shadowBtn.type = "button";
    shadowBtn.textContent = "Shadow";
    shadowBtn.style.cssText = "width:80px;height:28px";
    shadowBtn.addEventListener("click", (event) => { window.shadowClicked = true; window.shadowTrusted = event.isTrusted; });
    root.appendChild(shadowBtn);
    window.shadowBtn = shadowBtn;
    document.querySelector("#row-button").addEventListener("click", (event) => { window.rowClicked = true; window.rowTrusted = event.isTrusted; });
    document.querySelector("#pad-row").addEventListener("click", (event) => { window.padTarget = event.target.id || event.target.tagName; });
    document.querySelector("#pad-button").addEventListener("click", () => { window.padButtonClicked = true; });
    document.querySelector("#occluded-btn").addEventListener("click", () => { window.occludedClicked = true; });
    document.querySelector("#clipped-btn").addEventListener("click", () => { window.clippedClicked = true; });
    document.querySelector("#center-link").addEventListener("click", () => { window.centerClicked = true; });
    document.querySelector("#thread-hint").addEventListener("click", () => { window.threadHintClicked = true; });
    document.querySelector("#rich-editor").addEventListener("mousedown", (event) => { window.richClicked = true; window.richTrusted = event.isTrusted; window.richTarget = event.target.tagName; });
    document.querySelector("#rich-nested").addEventListener("mousedown", (event) => { window.richNestedClicked = true; window.richNestedTarget = event.target.id || event.target.tagName; });
    document.querySelector("#editor-button").addEventListener("click", (event) => { window.editorButtonClicked = true; window.editorButtonTrusted = event.isTrusted; });
    document.addEventListener("mousedown", (event) => {
      window.lastMouse = { x: event.clientX, y: event.clientY, id: event.target.id, tag: event.target.tagName, trusted: event.isTrusted };
      window.clickLog.push(window.lastMouse);
    }, true);
    document.addEventListener("keydown", (event) => {
      window.keysSeen.push({ key: event.key, ctrl: event.ctrlKey, shift: event.shiftKey, trusted: event.isTrusted });
    }, true);
    document.addEventListener("wheel", (event) => {
      window.wheelEvents.push({ id: event.target && event.target.id, deltaY: event.deltaY, trusted: event.isTrusted });
    }, true);
    window.addEventListener("keyup", (event) => {
      if (event.code === "KeyJ") window.jKeyups += 1;
    }, true);
    window.hintFor = (el) => {
      const overlay = document.querySelector("[data-dotfiles-hints]");
      if (!overlay || !overlay.shadowRoot || !el) return "";
      const rect = el.getBoundingClientRect();
      const targetLeft = Math.max(0, rect.left);
      const targetTop = Math.max(0, rect.top);
      const badge = [...overlay.shadowRoot.querySelectorAll("[data-hint]")].find((item) => {
        const box = item.getBoundingClientRect();
        return Math.abs(box.left - targetLeft) <= 3 && Math.abs(box.top - targetTop) <= 3;
      });
      return badge ? badge.dataset.hint : "";
    };
  })()`);
  const guardFocus = () => {
    try {
      const active = JSON.parse(execFileSync("hyprctl", ["activewindow", "-j"], { encoding: "utf8" }));
      const title = String(active.title || "");
      if (title === "slack-theme-smoke" || title === "Slack") throw new Error("smoke took keyboard focus: " + title);
    } catch (error) {
      if (String(error && error.message || error).includes("took keyboard focus")) throw error;
    }
  };
  const key = async (keyCode, modifiers = []) => {
    guardFocus();
    win.webContents.sendInputEvent({ type: "keyDown", keyCode, modifiers });
    win.webContents.sendInputEvent({ type: "keyUp", keyCode, modifiers });
    await sleep(80);
  };
  const activeId = () => win.webContents.executeJavaScript("document.activeElement && document.activeElement.id || ''");
  const pressGi = async () => {
    await key("G");
    await key("I");
    await sleep(180);
  };
  await win.webContents.executeJavaScript("document.querySelector('#thread-composer').focus()");
  assert.equal(await activeId(), "thread-composer");
  await key("Escape");
  assert.notEqual(await activeId(), "thread-composer", "thread escape did not leave the composer");
  await pressGi();
  assert.equal(await activeId(), "thread-composer", "thread composer escape then gi without a prior thread scroll");
  await win.webContents.executeJavaScript("document.querySelector('#primary-composer').focus()");
  await key("Escape");
  assert.notEqual(await activeId(), "primary-composer", "primary escape did not leave the composer");
  await pressGi();
  assert.equal(await activeId(), "primary-composer", "primary composer escape then gi without relying on scroll");
  assert.equal(await win.webContents.executeJavaScript(`(() => {
    const inline = document.querySelector("#inline-edit");
    return Boolean(inline.closest(".c-texty_input") && inline.closest(".c-message_kit__background") && !inline.closest(".p-message_input") && !inline.closest(".p-message_input_field"));
  })()`), true, "inline edit is a texty field inside message content");
  await win.webContents.executeJavaScript(`(() => {
    document.querySelector("#primary-composer").hidden = true;
    const sink = document.createElement("div");
    sink.id = "composer-hidden-sink";
    sink.tabIndex = -1;
    document.body.appendChild(sink);
    sink.focus();
  })()`);
  assert.equal(await activeId(), "composer-hidden-sink");
  await pressGi();
  assert.equal(await activeId(), "composer-hidden-sink", "a sole visible texty inline edit is not the composer");
  await win.webContents.executeJavaScript("document.querySelector('#primary-composer').hidden = false");
  await win.webContents.executeJavaScript("if (document.activeElement && document.activeElement.blur) document.activeElement.blur()");
  win.webContents.sendInputEvent({ type: "mouseMove", x: 8, y: 8 });
  await sleep(40);
  const scrollTop = () => win.webContents.executeJavaScript("document.querySelector('#scroller').scrollTop");
  const settled = async () => {
    let previous = await scrollTop();
    for (let attempt = 0; attempt < 20; attempt += 1) {
      await sleep(100);
      const next = await scrollTop();
      if (next === previous) return next;
      previous = next;
    }
    return previous;
  };
  const until = async (predicate) => {
    for (let attempt = 0; attempt < 20; attempt += 1) {
      const value = await scrollTop();
      if (predicate(value)) return value;
      await sleep(100);
    }
    return scrollTop();
  };
  const before = await settled();
  await key("J");
  const afterJ = await until((value) => value > before);
  assert.ok(afterJ > before, JSON.stringify(await win.webContents.executeJavaScript(`({
    before: ${before},
    after: document.querySelector('#scroller').scrollTop,
    height: document.querySelector('#scroller').scrollHeight,
    focused: document.hasFocus(),
    active: document.activeElement && document.activeElement.id,
    seen: window.keysSeen.slice(-4),
    injected: window.__slackDotfilesKeys === true
  })`)));
  await key("G", ["shift"]);
  assert.ok(await until((value) => value > 1500) > 1500, "G reaches the bottom of the loaded pane");
  await key("G");
  await key("G");
  assert.ok(await until((value) => value < 50) < 50, "gg returns to the top");
  const forbidden = await win.webContents.executeJavaScript("window.keysSeen.some((item) => ['Home','End','PageUp','PageDown'].includes(item.key))");
  assert.equal(forbidden, false);
  await win.webContents.executeJavaScript("document.querySelector('.p-top_nav__search').focus()");
  const searchTop = await settled();
  await key("J");
  await sleep(200);
  assert.equal(await settled(), searchTop);
  assert.equal(await win.webContents.executeJavaScript("document.activeElement.className"), "p-top_nav__search");
  await win.webContents.executeJavaScript("document.querySelector('#scroller').focus()");
  await key("G");
  await key("I");
  await sleep(200);
  const giProbe = await win.webContents.executeJavaScript(`(() => {
    const el = document.querySelector("textarea");
    const rect = el.getBoundingClientRect();
    const hit = document.elementFromPoint(rect.left + rect.width / 2, rect.top + rect.height / 2);
    return {
      active: document.activeElement && document.activeElement.tagName,
      activeId: document.activeElement && document.activeElement.id,
      inlineTop: document.querySelector('#inline-edit').getBoundingClientRect().top,
      focused: document.hasFocus(),
      seen: window.keysSeen.slice(-6),
      rect: { top: rect.top, left: rect.left, width: rect.width, height: rect.height },
      hit: hit && hit.tagName,
    };
  })()`);
  assert.equal(giProbe.active, "TEXTAREA", JSON.stringify({ giProbe, bridge: bridge.slice(-4) }));
  assert.equal(giProbe.activeId, "primary-composer", JSON.stringify(giProbe));
  assert.ok(giProbe.inlineTop < 700, JSON.stringify(giProbe));
  await win.webContents.executeJavaScript("document.querySelector('#thread-body').focus()");
  await key("G");
  await key("I");
  await sleep(200);
  assert.equal(await win.webContents.executeJavaScript("document.activeElement.id"), "thread-composer");
  await win.webContents.executeJavaScript("document.querySelector('#scroller').focus()");
  await key("F");
  await sleep(100);
  const hint = await win.webContents.executeJavaScript(`(() => {
    const host = document.querySelector("[data-dotfiles-hints]");
    const button = document.querySelector("#preview-button");
    const rect = button.getBoundingClientRect();
    const badge = [...host.shadowRoot.querySelectorAll("[data-hint]")].find((item) => {
      const box = item.getBoundingClientRect();
      return Math.abs(box.left - rect.left) < 30 && Math.abs(box.top - rect.top) < 30;
    });
    return badge ? badge.dataset.hint : "";
  })()`);
  assert.ok(hint, "preview button has a hint");
  for (const ch of hint) await key(ch.toUpperCase());
  await sleep(200);
  assert.equal(await win.webContents.executeJavaScript("window.clicked"), true, JSON.stringify(await win.webContents.executeJavaScript("({ clicked: window.clicked, lastMouse: window.lastMouse, ratio: window.devicePixelRatio })")));
  await key("F");
  await sleep(50);
  await key("Escape");
  assert.equal(await win.webContents.executeJavaScript("document.querySelector('[data-dotfiles-hints]')"), null);
  const seenBefore = await win.webContents.executeJavaScript("window.keysSeen.length");
  await key("/");
  await sleep(100);
  const seen = await win.webContents.executeJavaScript("window.keysSeen.slice(-8)");
  assert.equal(seen.some((item) => item.key === "/"), false, JSON.stringify(seen));
  assert.equal(seen.some((item) => item.ctrl && item.key.toLowerCase() === "g" && item.trusted), true, JSON.stringify(seen));
  const ctrlSlash = await win.webContents.executeJavaScript("window.keysSeen.filter((item) => item.ctrl && item.key.toLowerCase() === 'g').length");
  await key("Slash", ["control"]);
  await sleep(100);
  const ctrlSlashAfter = await win.webContents.executeJavaScript("window.keysSeen.filter((item) => item.ctrl && item.key.toLowerCase() === 'g').length");
  assert.equal(ctrlSlashAfter, ctrlSlash);
  assert.ok(seenBefore >= 0);

  const js = (source) => win.webContents.executeJavaScript(source);
  const activateHint = async (hint, label) => {
    assert.ok(hint, label || "hint missing");
    for (const ch of hint) await key(ch.toUpperCase());
    await sleep(200);
  };
  const openHints = async () => {
    await key("F");
    await sleep(120);
  };
  await js(`(() => {
    const inner = document.querySelector("#nested-scroll");
    const outer = document.querySelector("#scroller");
    inner.scrollTop = 0;
    inner.scrollLeft = 0;
    outer.scrollTop = 0;
    inner.focus();
  })()`);
  win.webContents.sendInputEvent({ type: "mouseMove", x: 8, y: 8 });
  await sleep(40);
  await key("J");
  await sleep(120);
  const focusedNested = await js(`({ inner: document.querySelector("#nested-scroll").scrollTop, outer: document.querySelector("#scroller").scrollTop })`);
  assert.ok(focusedNested.inner > 40, JSON.stringify(focusedNested));
  assert.equal(focusedNested.outer, 0, JSON.stringify(focusedNested));
  const nestedBox = await js(`(() => {
    const inner = document.querySelector("#nested-scroll");
    inner.scrollTop = 0;
    document.body.focus();
    const rect = inner.getBoundingClientRect();
    return { x: Math.round(rect.left + rect.width / 2), y: Math.round(rect.top + rect.height / 2) };
  })()`);
  win.webContents.sendInputEvent({ type: "mouseMove", x: nestedBox.x, y: nestedBox.y });
  await sleep(40);
  await key("J");
  await sleep(120);
  assert.ok(await js("document.querySelector('#nested-scroll').scrollTop") > 40, "hover j uses the nested scroller");
  await js("document.querySelector('#nested-scroll').scrollLeft = 0");
  await key("L");
  await sleep(120);
  assert.ok(await js("document.querySelector('#nested-scroll').scrollLeft") > 40, "l uses the nested horizontal scroller");
  await js(`document.querySelector("#scroller").scrollTop = 400; document.querySelector("#nested-scroll").scrollTop = 30;`);
  await key("G");
  await key("G");
  await sleep(200);
  const extremes = await js(`({ outer: document.querySelector("#scroller").scrollTop, inner: document.querySelector("#nested-scroll").scrollTop })`);
  assert.ok(extremes.outer < 50, JSON.stringify(extremes));
  assert.ok(extremes.inner > 20, JSON.stringify(extremes));
  const detachBox = await js(`(() => {
    const rect = document.querySelector("#nested-scroll").getBoundingClientRect();
    return { x: Math.round(rect.left + 8), y: Math.round(rect.top + 8) };
  })()`);
  win.webContents.sendInputEvent({ type: "mouseMove", x: detachBox.x, y: detachBox.y });
  await sleep(40);
  await js(`document.querySelector("#nested-scroll").remove(); document.body.focus(); document.querySelector("#scroller").scrollTop = 0;`);
  await key("J");
  await sleep(150);
  assert.ok(await scrollTop() > 40, "detached pointer falls back to the pane");

  await js("document.body.focus()");
  await openHints();
  const hintMap = await js(`({
    shadow: window.hintFor(window.shadowBtn),
    row: window.hintFor(document.querySelector("#row")),
    rowButton: window.hintFor(document.querySelector("#row-button")),
    clipped: window.hintFor(document.querySelector("#clipped-btn")),
    occluded: window.hintFor(document.querySelector("#occluded-btn")),
    preview: window.hintFor(document.querySelector("#preview-button"))
  })`);
  assert.ok(hintMap.shadow, JSON.stringify(hintMap));
  assert.ok(hintMap.rowButton, JSON.stringify(hintMap));
  assert.equal(hintMap.row, "", JSON.stringify(hintMap));
  assert.equal(hintMap.clipped, "", JSON.stringify(hintMap));
  assert.equal(hintMap.occluded, "", JSON.stringify(hintMap));
  assert.equal(await js("window.occludedClicked || window.clippedClicked || false"), false);
  await activateHint(hintMap.shadow, "shadow button hint");
  assert.equal(await js("window.shadowClicked === true && window.shadowTrusted === true"), true, "open-shadow hint did not deliver a trusted click");
  await openHints();
  await activateHint(await js("window.hintFor(document.querySelector('#row-button'))"), "nested button hint");
  assert.equal(await js("window.rowClicked === true && window.rowTrusted === true"), true);
  await openHints();
  await activateHint(await js("window.hintFor(document.querySelector('#rich-editor'))"), "rich text editor hint");
  assert.equal(await js("window.richClicked === true && window.richTrusted === true && (window.richTarget === 'P' || window.richTarget === 'DIV')"), true, "rich text hint did not activate the editor");
  await key("Escape");
  assert.equal(await js("Boolean(document.activeElement && document.activeElement.isContentEditable)"), false, "escape did not leave the rich editor");
  await openHints();
  assert.equal(await js("document.querySelector('[data-dotfiles-hints]') != null"), true, "hint overlay did not open after leaving the rich editor");
  await activateHint(await js("window.hintFor(document.querySelector('#editor-button'))"), "nested editor action hint");
  assert.equal(await js("window.editorButtonClicked === true && window.editorButtonTrusted === true"), true);
  await js("window.editorButtonClicked = false; window.richNestedClicked = false");
  await key("Escape");
  assert.equal(await js("Boolean(document.activeElement && (document.activeElement.isContentEditable || document.activeElement.closest('.ql-editor, [role=textbox]')))"), false, "escape did not leave the nested editor action");
  await openHints();
  assert.equal(await js("document.querySelector('[data-dotfiles-hints]') != null"), true, "hint overlay did not open for the editor host");
  await activateHint(await js("window.hintFor(document.querySelector('#rich-nested'))"), "editor host hint");
  assert.equal(await js("window.editorButtonClicked"), false, "editor hint activated the nested action");
  assert.equal(await js("window.richNestedClicked === true && window.richNestedTarget !== 'editor-button'"), true);
  await key("Escape");
  assert.equal(await js("Boolean(document.activeElement && document.activeElement.isContentEditable)"), false, "escape did not leave the nested editor");
  await openHints();
  assert.equal(await js("document.querySelector('[data-dotfiles-hints]') != null"), true, "hint overlay did not open after leaving the nested editor");
  const padHint = await js("window.hintFor(document.querySelector('#pad-row'))");
  await activateHint(padHint, "padded row hint");
  const pad = await js(`({ button: window.padButtonClicked === true, target: window.padTarget || "" })`);
  assert.equal(pad.button, false, JSON.stringify(pad));
  assert.notEqual(pad.target, "pad-button", JSON.stringify(pad));

  await js("document.querySelector('#primary-composer').focus()");
  await key("Escape");
  await js("document.body.focus()");
  await openHints();
  await activateHint(await js("window.hintFor(document.querySelector('#thread-hint'))"), "thread hint");
  assert.equal(await js("window.threadHintClicked"), true);
  await js(`(() => {
    const sink = document.createElement("div");
    sink.id = "focus-sink";
    sink.tabIndex = -1;
    document.body.appendChild(sink);
    sink.focus();
  })()`);
  assert.equal(await activeId(), "focus-sink");
  await pressGi();
  assert.equal(await activeId(), "thread-composer", "hint activation keeps the thread pane");

  await js(`(() => {
    window.clickLog = [];
    window.centerClicked = false;
    const editor = document.querySelector("#primary-composer");
    window.refocusEditor = () => { editor.focus(); };
    editor.addEventListener("blur", window.refocusEditor);
    editor.focus();
  })()`);
  await key("Escape");
  await sleep(150);
  const recovery = await js(`({
    clicks: window.clickLog.map((item) => ({ id: item.id, tag: item.tag })),
    center: window.centerClicked === true
  })`);
  assert.equal(recovery.center, false, JSON.stringify(recovery));
  assert.equal(recovery.clicks.some((item) => ["A", "BUTTON", "INPUT", "TEXTAREA", "SELECT"].includes(item.tag) || ["center-link", "preview-button", "row-button", "pad-button", "primary-composer", "inline-edit", "thread-composer"].includes(item.id)), false, JSON.stringify(recovery));
  await js(`(() => {
    window.clickLog = [];
    const pane = document.querySelector(".p-workspace__primary_view");
    pane.style.position = "relative";
    const cover = document.createElement("a");
    cover.id = "pane-cover";
    cover.href = "#cover";
    cover.style.cssText = "position:absolute;inset:0;z-index:20";
    pane.appendChild(cover);
    document.querySelector("#primary-composer").focus();
  })()`);
  await key("Escape");
  await sleep(150);
  assert.equal(await js("window.clickLog.some((item) => item.id === 'pane-cover' || item.tag === 'A' || item.tag === 'BUTTON' || item.tag === 'INPUT' || item.tag === 'TEXTAREA')"), false, "escape recovery clicked an actionable cover");
  await js(`(() => {
    document.querySelector("#pane-cover").remove();
    const editor = document.querySelector("#primary-composer");
    editor.removeEventListener("blur", window.refocusEditor);
    window.jKeyups = 0;
    document.body.focus();
  })()`);
  win.webContents.sendInputEvent({ type: "keyDown", keyCode: "J" });
  win.webContents.sendInputEvent({ type: "keyDown", keyCode: "J" });
  win.webContents.sendInputEvent({ type: "keyDown", keyCode: "J" });
  win.webContents.sendInputEvent({ type: "keyUp", keyCode: "J" });
  await sleep(80);
  await js("window.jKeyups = 0; document.querySelector('#primary-composer').focus()");
  win.webContents.sendInputEvent({ type: "keyUp", keyCode: "J" });
  await sleep(80);
  assert.equal(await js("window.jKeyups"), 1, "editable j keyup was swallowed after repeated holds");

  const paintStart = paints;
  const wheelStart = bridge.length;
  await setMotion("no-preference");
  await js(`(() => {
    const el = document.querySelector("#scroller");
    el.scrollTop = 0;
    el.focus();
    window.wheelEvents = [];
  })()`);
  win.webContents.sendInputEvent({ type: "mouseMove", x: 8, y: 8 });
  await sleep(40);
  const smoothBefore = await scrollTop();
  await key("J");
  let smoothTop = smoothBefore;
  for (let attempt = 0; attempt < 40; attempt += 1) {
    await sleep(50);
    smoothTop = await scrollTop();
    if (smoothTop > smoothBefore + 50) break;
  }
  await sleep(400);
  smoothTop = await scrollTop();
  const smoothWheels = bridge.slice(wheelStart).filter((message) => message.includes('"type":"wheel"'));
  const pageWheels = await js("window.wheelEvents.slice()");
  assert.ok(smoothTop > smoothBefore + 50 && smoothTop < smoothBefore + 160, JSON.stringify({ smoothBefore, smoothTop, smoothWheels, pageWheels, paints, paintStart }));
  assert.equal(smoothWheels.length, 0, JSON.stringify(smoothWheels));
  assert.equal(pageWheels.length, 0, JSON.stringify(pageWheels));
  assert.ok(paints > paintStart, "normal-motion scroll did not paint");
  await setMotion("reduce");
  const reducedStart = bridge.length;
  await js(`(() => {
    const el = document.querySelector("#scroller");
    el.scrollTop = 0;
    el.focus();
    window.wheelEvents = [];
  })()`);
  await key("J");
  await sleep(120);
  const reducedTop = await scrollTop();
  const reducedWheels = bridge.slice(reducedStart).filter((message) => message.includes('"type":"wheel"'));
  assert.ok(reducedTop > 50 && reducedTop < 160, JSON.stringify({ reducedTop, reducedWheels }));
  assert.equal(reducedWheels.length, 0, JSON.stringify(reducedWheels));

  await js("document.querySelector('#thread-composer').focus()");
  await key("Escape");
  assert.notEqual(await activeId(), "thread-composer");
  await pressGi();
  assert.equal(await activeId(), "thread-composer", "thread pane was current before it closed");
  await js(`(() => {
    const editor = document.querySelector("#thread-composer");
    if (editor && editor.blur) editor.blur();
    document.querySelector(".p-workspace__secondary_view").remove();
    if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  })()`);
  await pressGi();
  assert.equal(await activeId(), "primary-composer", "closed thread does not keep a stale pane");

  await win.loadURL("data:text/html,<body>Out of scope</body>");
  await sleep(300);
  assert.notEqual(await win.webContents.executeJavaScript("getComputedStyle(document.body).backgroundColor"), rgb(active.background));
  assert.equal(await win.webContents.executeJavaScript("window.__slackDotfilesKeys === true"), false);
  console.log("PASS: origin boundary, CSS input validation, active palette/fonts, focus, live switch, reload, invalid-theme recovery, unthemed navigation, keymap");
  app.exit(0);
})().catch((error) => { console.error(error); app.exit(1); });
