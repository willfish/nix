"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const keys = require("./keys.cjs");
const theme = require("./theme.cjs");
const { patch } = require("./patch.cjs");

const source = fs.readFileSync(path.join(__dirname, "keys.cjs"), "utf8");

function press(state, key, ctx = {}, extra = {}) {
  return keys.reduce(state, {
    type: "keydown",
    key,
    code: extra.code || (key.length === 1 ? `Key${key.toUpperCase()}` : key),
    shift: Boolean(extra.shift),
    ctrl: Boolean(extra.ctrl),
    alt: Boolean(extra.alt),
    meta: Boolean(extra.meta),
    repeat: Boolean(extra.repeat),
    composing: Boolean(extra.composing),
    time: extra.time || 0,
  }, { allowed: true, documentFocused: true, editable: false, menu: false, dialog: false, emoji: false, ...ctx });
}

function prefixFree(hints) {
  return hints.every((hint, index) => hints.every((other, otherIndex) => index === otherIndex || !other.startsWith(hint)));
}

test("keys switch ignores the theme switch", () => {
  assert.equal(keys.enabled({ SLACK_DOTFILES_KEYS: "0", SLACK_DOTFILES_THEME: "1" }), false);
  assert.equal(keys.enabled({ SLACK_DOTFILES_THEME: "0" }), true);
  assert.equal(keys.enabled({}), true);
  assert.equal(source.includes("SLACK_DOTFILES_THEME"), false);
});

test("allowlist is the theme function, not a second host check", () => {
  const allowed = keys.loadAllowedURL((name) => {
    if (name === "./dotfiles-theme.cjs") throw new Error("packaged name");
    if (name === "./theme.cjs") return theme;
    throw new Error(name);
  });
  assert.equal(allowed, theme.allowedURL);
  assert.equal(allowed("https://app.slack.com/client/test"), true);
  assert.equal(allowed("https://example.slack.com/client/test"), true);
  assert.equal(allowed("https://slack.com.evil.example/"), false);
  assert.equal(allowed("https://evilslack.com/"), false);
  assert.equal(allowed("file:///etc/passwd"), false);
  assert.equal(allowed("http://app.slack.com/"), false);
  assert.equal(source.includes("hostname"), false);
  assert.equal(source.includes("slack.com"), false);
});

test("counts, repeats, half-page, and gg/G direction", () => {
  let step = press(keys.initialState(), "3");
  step = press(step.state, "j", {}, { time: 10 });
  assert.equal(step.effects[0].motion, "j");
  assert.equal(step.effects[0].count, 3);
  const repeated = press(step.state, "j", {}, { repeat: true, time: 20 });
  assert.equal(repeated.effects[0].count, 1);
  const native = { scrollHeight: 2000, clientHeight: 200, scrollWidth: 400, clientWidth: 400 };
  assert.equal(keys.scrollPlan(native, "j", 3, false).top, 300);
  assert.equal(keys.scrollPlan(native, "j", 1, false).top, 100);
  assert.equal(keys.scrollPlan(native, "d", 2, false).top, 200);
  assert.equal(keys.scrollPlan(native, "u", 1, false).top, -100);
  let top = press(press(keys.initialState(), "5").state, "g", {}, { time: 1 });
  top = press(top.state, "g", {}, { time: 2 });
  assert.equal(top.effects[0].motion, "gg");
  assert.equal(top.effects[0].count, 1);
  const bottom = press(keys.initialState(), "G", {}, { shift: true, code: "KeyG" });
  assert.equal(bottom.effects[0].motion, "G");
  assert.equal(keys.scrollPlan(native, "gg", 9, false).method, "scrollTo");
  assert.equal(keys.scrollPlan(native, "gg", 9, false).top, 0);
  assert.equal(keys.scrollPlan(native, "G", 9, false).top, 1800);
});

test("g then j scrolls, and a stale g does not swallow the next key", () => {
  const chord = press(press(keys.initialState(), "g").state, "j", {}, { time: 100 });
  assert.equal(chord.effects[0].motion, "j");
  const stale = press(press(keys.initialState(), "g", {}, { time: 0 }).state, "j", {}, { time: 1001 });
  assert.equal(stale.effects[0].motion, "j");
  assert.equal(stale.state.prefix, "");
});

test("search is trusted Ctrl+G and does not steal composer or ctrl chords", () => {
  const search = press(keys.initialState(), "/", {}, { code: "Slash" });
  assert.equal(search.prevent, true);
  assert.deepEqual(search.effects, [{ type: "key", keyCode: "G", modifiers: ["control"] }]);
  const typed = press(keys.initialState(), "/", { editable: true }, { code: "Slash" });
  assert.equal(typed.prevent, false);
  assert.deepEqual(typed.effects, []);
  for (const [key, extra] of [["/", { ctrl: true, code: "Slash" }], ["g", { ctrl: true, code: "KeyG" }], ["F", { shift: true, code: "KeyF" }], ["?", { shift: true, code: "Slash" }]]) {
    const passed = press(keys.initialState(), key, {}, extra);
    assert.equal(passed.prevent, false, key);
    assert.deepEqual(passed.effects, [], key);
  }
});

test("escape cancels hints, blurs the composer, and leaves menus alone", () => {
  const hinted = press(keys.initialState(), "f");
  const cancelled = press(hinted.state, "Escape", {}, { code: "Escape" });
  assert.equal(cancelled.prevent, true);
  assert.equal(cancelled.state.mode, "normal");
  assert.equal(cancelled.effects.some((effect) => effect.type === "key"), false);
  assert.equal(keys.escAction(hinted.state, {}), "cancel-hint");
  const blurred = press(keys.initialState(), "Escape", { editable: true }, { code: "Escape" });
  assert.equal(blurred.prevent, true);
  assert.equal(blurred.effects[0].type, "blur");
  assert.equal(keys.escAction(keys.initialState(), { editable: true }), "blur");
  for (const ctx of [{ menu: true, editable: true }, { dialog: true }, { emoji: true, editable: true }]) {
    const passed = press(keys.initialState(), "Escape", ctx, { code: "Escape" });
    assert.equal(passed.prevent, false);
    assert.equal(keys.escAction(keys.initialState(), ctx), "pass");
  }
  const dialogHint = press(keys.initialState(), "f", { dialog: true });
  assert.equal(dialogHint.state.mode, "hint");
  assert.equal(dialogHint.effects[0].scope, "dialog");
});

test("gi uses the current pane composer and never search", () => {
  const composers = [
    { id: "primary", pane: "primary", visible: true, ql: true, search: false, message: true },
    { id: "thread", pane: "secondary", visible: true, ql: true, search: false, message: true },
    { id: "search", pane: "primary", visible: true, ql: true, search: true, message: false },
  ];
  assert.equal(keys.composerTarget({ focusPane: "secondary", composers }).id, "thread");
  assert.equal(keys.composerTarget({ focusPane: "primary", composers: [composers[2]] }), null);
  const focused = press(press(keys.initialState(), "g").state, "i", {}, { time: 20 });
  assert.equal(focused.effects[0].type, "focus-composer");
});

test("gi skips an earlier inline editor and fails closed when ambiguous", () => {
  const panes = { primary: { id: "p" }, secondary: { id: "s" } };
  const composers = [
    { id: "inline", pane: "primary", visible: true, ql: true, search: false, message: false },
    { id: "primary", pane: "primary", visible: true, ql: true, search: false, message: true },
    { id: "later-inline", pane: "primary", visible: true, ql: true, search: false, message: false },
    { id: "search", pane: "primary", visible: true, textbox: true, search: true, message: true },
  ];
  assert.equal(keys.composerTarget({ focusPane: "primary", panes, composers }).id, "primary");
  assert.equal(keys.composerTarget({
    focusPane: "primary",
    panes,
    composers: composers.concat([{ id: "other", pane: "primary", visible: true, ql: true, search: false, message: true }]),
  }), null);
  assert.equal(keys.composerTarget({ focusPane: "primary", panes, composers: [composers[3]] }), null);
});

test("current pane follows focus or last pane and drops a closed thread", () => {
  const panes = { primary: { id: "p" }, secondary: { id: "s" } };
  assert.equal(keys.currentPane({ lastPane: "secondary", panes }), "secondary");
  assert.equal(keys.currentPane({ lastPane: "primary", panes }), "primary");
  assert.equal(keys.currentPane({ focusPane: "secondary", lastPane: "primary", panes }), "secondary");
  assert.equal(keys.currentPane({ focusPane: "secondary", lastPane: "secondary", panes: { primary: { id: "p" }, secondary: null } }), "primary");
  assert.equal(keys.currentPane({ lastPane: "secondary", panes: { primary: null, secondary: null } }), null);
});

test("scroll target prefers a live hover scroller, then focus, and gg stays on the pane", () => {
  const panes = { primary: { id: "pane" }, secondary: { id: "thread" } };
  assert.equal(keys.selectScrollTarget({
    panes,
    pointerScrollers: { x: { id: "wide" }, y: { id: "tall" } },
    activeScroller: { id: "focused" },
  }, "j").id, "tall");
  assert.equal(keys.selectScrollTarget({
    panes,
    pointerScrollers: { x: { id: "wide" }, y: { id: "tall" } },
  }, "l").id, "wide");
  assert.equal(keys.selectScrollTarget({
    panes,
    pointerScroller: { id: "gone", detached: true },
    activeScroller: { id: "focused" },
  }, "j").id, "focused");
  assert.equal(keys.selectScrollTarget({
    lastPane: "primary",
    panes,
    pointerScroller: { id: "hover" },
  }, "gg").id, "pane");
  assert.equal(keys.selectScrollTarget({
    lastPane: "secondary",
    panes: { primary: { id: "pane" }, secondary: null },
    pointerScroller: { id: "hover" },
  }, "G").id, "pane");
});

test("smooth scroll never wheel-falls-back before the animation moves", () => {
  const native = { scrollHeight: 2000, clientHeight: 200, scrollWidth: 400, clientWidth: 400 };
  const smooth = keys.scrollPlan(native, "j", 1, false);
  const instant = keys.scrollPlan(native, "j", 1, true);
  assert.equal(smooth.behavior, "smooth");
  assert.equal(keys.needsWheelFallback(smooth, { top: 0, left: 0 }, { top: 0, left: 0 }), false);
  assert.equal(instant.behavior, "auto");
  assert.equal(keys.needsWheelFallback(instant, { top: 0, left: 0 }, { top: 0, left: 0 }), true);
  assert.equal(keys.needsWheelFallback(instant, { top: 0, left: 0 }, { top: 100, left: 0 }), false);
  assert.equal(keys.needsWheelFallback(keys.scrollPlan(native, "G", 1, true), { top: 0, left: 0 }, { top: 0, left: 0 }), false);
});

function lift(state, key, ctx = {}, extra = {}) {
  return keys.reduce(state, {
    type: "keyup",
    key,
    code: extra.code || (key.length === 1 ? `Key${key.toUpperCase()}` : key),
    shift: false,
    ctrl: false,
    alt: false,
    meta: false,
    repeat: false,
    composing: Boolean(extra.composing),
    time: extra.time || 0,
  }, { allowed: true, documentFocused: true, editable: false, menu: false, dialog: false, emoji: false, ...ctx });
}

test("one keyup releases repeated holds and does not swallow a later editable keyup", () => {
  let state = press(keys.initialState(), "j").state;
  state = press(state, "j", {}, { repeat: true, time: 10 }).state;
  state = press(state, "j", {}, { repeat: true, time: 20 }).state;
  assert.deepEqual(state.swallowed, ["KeyJ"]);
  const released = lift(state, "j");
  assert.equal(released.prevent, true);
  assert.deepEqual(released.state.swallowed, []);
  const editableUp = lift(released.state, "j", { editable: true });
  assert.equal(editableUp.prevent, false);
  assert.deepEqual(editableUp.state.swallowed, []);
  const composing = press(keys.initialState(), "j", {}, { composing: true });
  assert.equal(composing.prevent, false);
  assert.deepEqual(composing.state.swallowed, []);
  assert.equal(lift(composing.state, "j", {}, { composing: true }).prevent, false);
  const held = press(keys.initialState(), "j").state;
  const reset = press(held, "x", {}, { ctrl: true, code: "KeyX" });
  assert.equal(reset.prevent, false);
  assert.deepEqual(reset.state.swallowed, []);
  assert.equal(lift(reset.state, "j").prevent, false);
});

test("hints are prefix-free, charset-only, and ordered sidebar then pane then send", () => {
  const hints = keys.assignHints(40, keys.HINT_CHARS);
  assert.equal(hints.length, 40);
  assert.equal(prefixFree(hints), true);
  assert.equal(hints.every((hint) => [...hint].every((ch) => keys.HINT_CHARS.includes(ch))), true);
  const ordered = keys.orderHintTargets([
    { group: "current", index: 0, id: "msg" },
    { group: "sidebar", index: 1, id: "side" },
    { group: "send", index: 2, id: "send" },
  ]);
  assert.deepEqual(ordered.map((item) => item.id), ["side", "msg", "send"]);
  const matched = keys.matchHints(hints, hints[3]);
  assert.equal(matched.activateIndex, 3);
  assert.equal(keys.matchHints(hints, "").activateIndex, null);
});

test("native scroll plans never use wheel, home, or end", () => {
  const native = keys.scrollPlan({ scrollHeight: 2000, clientHeight: 200, scrollWidth: 100, clientWidth: 100 }, "j", 1, false);
  assert.equal(native.method, "scrollBy");
  assert.equal(native.behavior, "smooth");
  assert.equal(JSON.stringify(native).includes("Home"), false);
  assert.equal(JSON.stringify(native).includes("End"), false);
  assert.equal(JSON.stringify(native).includes("wheel"), false);
  const reduced = keys.scrollPlan({ scrollHeight: 2000, clientHeight: 200, scrollWidth: 100, clientWidth: 100 }, "G", 1, true);
  assert.equal(reduced.behavior, "auto");
  assert.equal(reduced.method, "scrollTo");
  const wheel = keys.scrollPlan({ scrollHeight: 20, clientHeight: 20, scrollWidth: 20, clientWidth: 20 }, "j", 1, false);
  assert.equal(wheel.method, "wheel");
  assert.equal(wheel.deltaY, 100);
  assert.equal(JSON.stringify(wheel).includes("Home"), false);
  assert.equal(JSON.stringify(wheel).includes("Page"), false);
});

test("page source embeds the reducer and the bridge token", () => {
  const sourceText = keys.pageSource("token-1");
  assert.equal(sourceText.includes(keys.reduce.toString()), true);
  assert.equal(sourceText.includes(keys.needsWheelFallback.toString()), true);
  assert.equal(sourceText.includes(JSON.stringify("token-1")), true);
  assert.equal(sourceText.includes('.closest(".p-message_input, .p-message_input_field")'), true);
  assert.equal(sourceText.includes(".c-message_kit__background"), true);
  assert.equal(sourceText.includes(".c-message_kit__text"), true);
  assert.equal(sourceText.includes(".c-texty_input"), false);
  assert.equal(sourceText.includes("SLACK_DOTFILES_KEYS"), true);
  assert.equal(sourceText.includes("event.message"), false);
});

test("patch copies both modules and refuses a bad or second patch", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "slack-keys-patch-"));
  fs.mkdirSync(path.join(root, "dist"));
  fs.writeFileSync(path.join(root, "package.json"), JSON.stringify({ main: "./dist/boot.bundle.cjs" }));
  fs.writeFileSync(path.join(root, "dist/boot.bundle.cjs"), "original");
  const themePath = path.join(root, "theme.cjs");
  const keysPath = path.join(root, "keys.cjs");
  fs.writeFileSync(themePath, "theme");
  fs.writeFileSync(keysPath, "keys");
  patch(root, themePath, keysPath);
  const boot = fs.readFileSync(path.join(root, "dist/boot.bundle.cjs"), "utf8");
  assert.equal(boot.startsWith('require("./dotfiles-theme.cjs").install();\nrequire("./dotfiles-keys.cjs").install();\n'), true);
  assert.equal(fs.readFileSync(path.join(root, "dist/dotfiles-theme.cjs"), "utf8"), "theme");
  assert.equal(fs.readFileSync(path.join(root, "dist/dotfiles-keys.cjs"), "utf8"), "keys");
  assert.throws(() => patch(root, themePath, keysPath));
  const wrong = fs.mkdtempSync(path.join(os.tmpdir(), "slack-keys-main-"));
  fs.writeFileSync(path.join(wrong, "package.json"), JSON.stringify({ main: "./index.js" }));
  assert.throws(() => patch(wrong, themePath, keysPath));
});
