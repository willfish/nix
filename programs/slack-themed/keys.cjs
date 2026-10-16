"use strict";

// Vimium-shaped keys for the desktop client. / is workspace search, not
// find-in-page: Slack already has an index, and Ctrl+/ must stay help.
// gg/G move the loaded pane, not the channel's full history.

const HINT_CHARS = "sadjklewcmpgh";

function enabled(env) {
  return !env || env.SLACK_DOTFILES_KEYS !== "0";
}

function loadAllowedURL(requireFn) {
  for (const name of ["./dotfiles-theme.cjs", "./theme.cjs"]) {
    try {
      const loaded = requireFn(name);
      if (loaded && typeof loaded.allowedURL === "function") return loaded.allowedURL;
    } catch {
      // The packaged name and the source-tree name are different files.
    }
  }
  return null;
}

function initialState() {
  return { mode: "normal", count: 0, prefix: "", prefixAt: 0, query: "", swallowed: [] };
}

function cloneState(state) {
  return {
    mode: state.mode,
    count: state.count,
    prefix: state.prefix,
    prefixAt: state.prefixAt,
    query: state.query,
    swallowed: state.swallowed.slice(),
  };
}

function swallow(state, input, effects) {
  if (input.code) state.swallowed.push(input.code);
  return { state, prevent: true, effects };
}

function currentPane(ctx) {
  if (!ctx) return null;
  if (ctx.focusPane === "primary" || ctx.focusPane === "secondary") return ctx.focusPane;
  if (ctx.lastPane === "primary" || ctx.lastPane === "secondary") return ctx.lastPane;
  if (ctx.panes && ctx.panes.primary) return "primary";
  return null;
}

function selectScrollTarget(ctx, motion) {
  const pane = currentPane(ctx);
  const paneTarget = pane && ctx && ctx.panes ? ctx.panes[pane] : null;
  if (motion === "gg" || motion === "G" || motion === "gi") return paneTarget || null;
  if (ctx && ctx.pointerScroller) return ctx.pointerScroller;
  if (ctx && ctx.activeScroller) return ctx.activeScroller;
  return paneTarget || null;
}

function scrollPlan(target, motion, count, reducedMotion) {
  const behavior = reducedMotion ? "auto" : "smooth";
  const keys = [];
  if (!target) return { method: "none", behavior, keys };
  if (motion === "gg") return { method: "scrollTo", top: 0, left: 0, behavior, keys };
  if (motion === "G") {
    return {
      method: "scrollTo",
      top: Math.max(0, (target.scrollHeight || 0) - (target.clientHeight || 0)),
      left: 0,
      behavior,
      keys,
    };
  }
  const n = Math.max(1, count || 1);
  let top = 0;
  let left = 0;
  if (motion === "j") top = n * 100;
  else if (motion === "k") top = -n * 100;
  else if (motion === "l") left = n * 100;
  else if (motion === "h") left = -n * 100;
  else if (motion === "d") top = n * Math.floor((target.clientHeight || 0) / 2);
  else if (motion === "u") top = -n * Math.floor((target.clientHeight || 0) / 2);
  else return { method: "none", behavior, keys };
  const native = (target.scrollHeight || 0) > (target.clientHeight || 0) + 1
    || (target.scrollWidth || 0) > (target.clientWidth || 0) + 1;
  if (!native) return { method: "wheel", deltaX: left, deltaY: top, behavior, keys };
  return { method: "scrollBy", top, left, behavior, keys };
}

function composerTarget(ctx) {
  const pane = currentPane(ctx);
  if (!pane || !ctx || !ctx.composers) return null;
  const list = ctx.composers.filter((item) => item && item.pane === pane && item.visible && !item.search);
  return list.find((item) => item.ql) || list.find((item) => item.textbox || item.textarea) || null;
}

function assignHints(count, chars) {
  const alphabet = String(chars || "");
  if (!alphabet || count <= 0) return [];
  const hints = [""];
  let offset = 0;
  while (hints.length - offset < count || hints.length === 1) {
    const hint = hints[offset];
    offset += 1;
    for (const ch of alphabet) hints.push(hint + ch);
    if (offset > 10000) break;
  }
  return hints.slice(offset, offset + count).reverse();
}

function matchHints(assigned, query) {
  const normalized = String(query || "").toLowerCase();
  const matches = [];
  for (let index = 0; index < assigned.length; index += 1) {
    if (String(assigned[index]).toLowerCase().startsWith(normalized)) matches.push({ hint: assigned[index], index });
  }
  return { query: normalized, matches, activateIndex: matches.length === 1 ? matches[0].index : null };
}

function escAction(state, ctx) {
  if (state && state.mode === "hint") return "cancel-hint";
  if (ctx && (ctx.menu || ctx.dialog || ctx.emoji)) return "pass";
  if (ctx && ctx.editable) return "blur";
  return "pass";
}

function orderHintTargets(targets) {
  const rank = { sidebar: 0, current: 1, other: 2, send: 3 };
  return targets.map((target, index) => ({ ...target, index: target.index == null ? index : target.index }))
    .sort((a, b) => (rank[a.group] ?? 2) - (rank[b.group] ?? 2) || a.index - b.index);
}

function reduceHint(state, input) {
  const effects = [];
  if (input.repeat) return swallow(state, input, effects);
  if (input.key === "Escape") {
    state.mode = "normal";
    state.query = "";
    effects.push({ type: "hint-cancel" });
    return swallow(state, input, effects);
  }
  if (input.key === "Backspace") {
    state.query = state.query.slice(0, -1);
    effects.push({ type: "hint-query", query: state.query });
    return swallow(state, input, effects);
  }
  if (input.key === "Enter") {
    effects.push({ type: "hint-activate", query: state.query });
    state.mode = "normal";
    state.query = "";
    return swallow(state, input, effects);
  }
  const ch = input.key && input.key.length === 1 ? input.key.toLowerCase() : "";
  if (ch && "sadjklewcmpgh".includes(ch) && !input.shift) {
    state.query += ch;
    effects.push({ type: "hint-query", query: state.query });
    return swallow(state, input, effects);
  }
  state.mode = "normal";
  state.query = "";
  effects.push({ type: "hint-cancel" });
  return swallow(state, input, effects);
}

function reduceNormal(state, input, effects) {
  if (state.prefix === "g" && input.time - state.prefixAt > 1000) {
    state.prefix = "";
    state.prefixAt = 0;
  }
  const key = input.key || "";
  const letter = key.length === 1 ? key.toLowerCase() : key;
  const shifted = Boolean(input.shift);

  if (!shifted && !input.repeat && /^[0-9]$/.test(key)) {
    if (key === "0" && state.count === 0) return { state, prevent: false, effects };
    const count = state.count * 10 + Number(key);
    if (count > 99) return swallow(state, input, effects);
    state.count = count;
    return swallow(state, input, effects);
  }

  if (letter === "g" && shifted && !input.repeat) {
    state.count = 0;
    state.prefix = "";
    effects.push({ type: "scroll", motion: "G", count: 1 });
    return swallow(state, input, effects);
  }

  if (letter === "g" && !shifted && !input.repeat) {
    if (state.prefix === "g") {
      state.prefix = "";
      state.count = 0;
      effects.push({ type: "scroll", motion: "gg", count: 1 });
      return swallow(state, input, effects);
    }
    state.prefix = "g";
    state.prefixAt = input.time || 0;
    state.count = 0;
    return swallow(state, input, effects);
  }

  if (state.prefix === "g") {
    state.prefix = "";
    if (letter === "i" && !shifted && !input.repeat) {
      state.count = 0;
      effects.push({ type: "focus-composer" });
      return swallow(state, input, effects);
    }
    return reduceNormal(state, input, effects);
  }

  if (letter === "f" && !shifted && !input.repeat) {
    state.count = 0;
    state.mode = "hint";
    state.query = "";
    effects.push({ type: "hint-open", scope: "window" });
    return swallow(state, input, effects);
  }

  if (key === "/" && !shifted && !input.repeat) {
    state.count = 0;
    effects.push({ type: "key", keyCode: "G", modifiers: ["control"] });
    return swallow(state, input, effects);
  }

  if (key === "Escape") return { state, prevent: false, effects };

  const motion = !shifted ? { j: "j", k: "k", h: "h", l: "l", d: "d", u: "u" }[letter] : null;
  if (motion) {
    const count = input.repeat ? 1 : (state.count || 1);
    state.count = 0;
    effects.push({ type: "scroll", motion, count });
    return swallow(state, input, effects);
  }

  return { state, prevent: false, effects };
}

function reduce(state, input, ctx) {
  const next = cloneState(state || initialState());
  const view = ctx || {};
  if (view.allowed === false || view.documentFocused === false) {
    return { state: next, prevent: false, effects: [] };
  }
  if (input.type === "keyup") {
    const index = next.swallowed.indexOf(input.code);
    if (index === -1) return { state: next, prevent: false, effects: [] };
    next.swallowed.splice(index, 1);
    return { state: next, prevent: true, effects: [] };
  }
  if (input.composing || input.ctrl || input.alt || input.meta) {
    const effects = [];
    if (next.mode === "hint") effects.push({ type: "hint-cancel" });
    next.mode = "normal";
    next.query = "";
    next.prefix = "";
    next.count = 0;
    return { state: next, prevent: false, effects };
  }
  if (next.mode === "hint") return reduceHint(next, input);
  if (view.editable) {
    next.prefix = "";
    next.count = 0;
    if (input.key === "Escape" && !view.menu && !view.dialog && !view.emoji) {
      return swallow(next, input, [{ type: "blur" }]);
    }
    return { state: next, prevent: false, effects: [] };
  }
  if (view.menu || view.emoji) {
    next.prefix = "";
    next.count = 0;
    return { state: next, prevent: false, effects: [] };
  }
  if (view.dialog) {
    next.prefix = "";
    next.count = 0;
    if (input.key === "f" && !input.shift && !input.repeat) {
      next.mode = "hint";
      next.query = "";
      return swallow(next, input, [{ type: "hint-open", scope: "dialog" }]);
    }
    return { state: next, prevent: false, effects: [] };
  }
  return reduceNormal(next, input, []);
}

const PAGE = `
const nodes = new Map();
let seq = 0;
let state = initialState();
let assigned = [];
let lastPane = null;
let pointer = null;
let host = null;

function report(payload) {
  console.debug("SLACK_DOTFILES_KEYS\\t" + TOKEN + "\\t" + JSON.stringify(payload));
}

function tag(el) {
  if (!el) return null;
  let id = el.getAttribute("data-dotfiles-key-id");
  if (!id) {
    seq += 1;
    id = "k" + seq;
    el.setAttribute("data-dotfiles-key-id", id);
  }
  nodes.set(id, el);
  return id;
}

function dims(el) {
  return {
    id: tag(el),
    scrollHeight: el.scrollHeight,
    clientHeight: el.clientHeight,
    scrollWidth: el.scrollWidth,
    clientWidth: el.clientWidth,
  };
}

function visible(el) {
  if (!el || el.hidden || el.getAttribute("aria-hidden") === "true" || el.inert) return false;
  const style = getComputedStyle(el);
  if (style.display === "none" || style.visibility === "hidden" || style.pointerEvents === "none") return false;
  if (Number(style.opacity) === 0) return false;
  const rect = el.getBoundingClientRect();
  return rect.width >= 4 && rect.height >= 4 && rect.width * rect.height >= 16
    && rect.bottom > 0 && rect.right > 0 && rect.top < innerHeight && rect.left < innerWidth;
}

function editable(el) {
  if (!el) return false;
  if (el.isContentEditable) return true;
  const tag = el.tagName;
  if (tag === "TEXTAREA" || tag === "SELECT") return true;
  if (tag === "INPUT") {
    const type = (el.type || "text").toLowerCase();
    return !["button", "checkbox", "radio", "submit", "file", "reset", "image", "hidden"].includes(type);
  }
  return Boolean(el.closest("[role=textbox], [role=searchbox], .ql-editor"));
}

function openDialog() {
  return [...document.querySelectorAll("[role=dialog], .c-dialog")].find(visible) || null;
}

function overlayOpen(selector) {
  return [...document.querySelectorAll(selector)].some(visible);
}

function paneOf(el) {
  if (!el) return null;
  if (el.closest(".p-workspace__secondary_view, .p-threads_view")) return "secondary";
  if (el.closest(".p-workspace__primary_view, .p-message_pane")) return "primary";
  return null;
}

function paneElement(name) {
  if (name === "secondary") return document.querySelector(".p-workspace__secondary_view, .p-threads_view");
  return document.querySelector(".p-workspace__primary_view, .p-message_pane");
}

function scrollerIn(root) {
  if (!root) return null;
  const marked = root.querySelector(".c-virtual_list__scroll_container");
  if (marked && (marked.scrollHeight > marked.clientHeight + 1 || marked.scrollWidth > marked.clientWidth + 1)) return marked;
  if (root.scrollHeight > root.clientHeight + 1 || root.scrollWidth > root.clientWidth + 1) return root;
  return marked || root;
}

function overflowAt(x, y) {
  const el = document.elementFromPoint(x, y);
  let node = el;
  let best = null;
  while (node && node !== document.documentElement) {
    if (node.scrollHeight > node.clientHeight + 1 || node.scrollWidth > node.clientWidth + 1) best = node;
    node = node.parentElement;
  }
  return best;
}

function buildCtx() {
  const focus = document.activeElement;
  const dialog = openDialog();
  const primary = paneElement("primary");
  const secondary = paneElement("secondary");
  const composers = [];
  for (const el of document.querySelectorAll(".ql-editor, [role=textbox], textarea")) {
    const search = Boolean(el.closest(".p-top_nav, [role=search]")) || el.getAttribute("type") === "search";
    composers.push({
      id: tag(el),
      pane: paneOf(el) || "primary",
      visible: visible(el),
      search,
      ql: el.classList.contains("ql-editor"),
      textbox: el.getAttribute("role") === "textbox",
      textarea: el.tagName === "TEXTAREA",
    });
  }
  return {
    allowed: true,
    documentFocused: true,
    editable: editable(focus),
    menu: overlayOpen("[role=menu], [role=listbox], .c-menu"),
    dialog: Boolean(dialog),
    emoji: overlayOpen(".p-emoji_picker, .c-emoji_picker"),
    focusPane: paneOf(focus),
    lastPane,
    panes: {
      primary: primary ? dims(scrollerIn(primary)) : null,
      secondary: secondary ? dims(scrollerIn(secondary)) : null,
    },
    composers,
    pointerScroller: pointer ? dims(pointer) : null,
    hints: assigned.map((item) => item.hint),
    dialog,
  };
}

function clearOverlay() {
  assigned = [];
  if (host) host.remove();
  host = null;
}

function renderHints(query) {
  if (!host) return;
  const root = host.shadowRoot;
  root.textContent = "";
  const q = (query || "").toLowerCase();
  for (const item of assigned) {
    if (q && !item.hint.startsWith(q)) continue;
    const rect = item.el.getBoundingClientRect();
    const badge = document.createElement("div");
    badge.dataset.hint = item.hint;
    badge.textContent = item.hint;
    badge.style.cssText = "position:fixed;left:" + Math.max(0, rect.left) + "px;top:" + Math.max(0, rect.top)
      + "px;background:#fff3a0;color:#000;font:12px/1 sans-serif;padding:1px 3px;border-radius:2px;z-index:2147483647;";
    if (q) badge.innerHTML = "<u>" + q + "</u>" + item.hint.slice(q.length);
    root.appendChild(badge);
  }
}

function hintable(el) {
  if (!visible(el) || el.disabled) return false;
  if (el.closest("[data-dotfiles-hints]")) return false;
  const role = el.getAttribute("role");
  const tag = el.tagName;
  if (tag === "A" && el.hasAttribute("href")) return true;
  if (tag === "BUTTON" || tag === "SUMMARY" || tag === "LABEL") return true;
  if (tag === "INPUT" && !el.disabled) return true;
  if (el.classList.contains("ql-editor")) return true;
  if (["button", "link", "menuitem", "tab", "option", "checkbox", "switch"].includes(role)) return true;
  return el.tabIndex >= 0 && tag !== "BODY";
}

function groupOf(el, pane) {
  const label = (el.getAttribute("aria-label") || el.textContent || "").trim().toLowerCase();
  if (label === "send" || label.startsWith("send ")) return "send";
  if (el.closest(".p-channel_sidebar, .p-tab_rail, .p-team_sidebar")) return "sidebar";
  if (pane && pane.contains(el)) return "current";
  return "other";
}

function collect(scope) {
  const dialog = scope === "dialog" ? openDialog() : null;
  const root = dialog || document.body;
  const paneName = currentPane({ focusPane: paneOf(document.activeElement), lastPane, panes: { primary: true } });
  const pane = paneElement(paneName) || paneElement("primary");
  const found = [];
  const walk = (node) => {
    if (!node || node.nodeType !== 1) return;
    if (hintable(node)) found.push(node);
    if (node.shadowRoot) walk(node.shadowRoot);
    for (const child of node.children || []) walk(child);
  };
  walk(root);
  const targets = found.map((el, index) => ({ el, group: groupOf(el, pane), index }));
  const ordered = orderHintTargets(targets);
  const hints = assignHints(ordered.length, HINT_CHARS);
  assigned = ordered.map((item, index) => ({ el: item.el, hint: hints[index] }));
  host = document.createElement("div");
  host.setAttribute("data-dotfiles-hints", "");
  host.setAttribute("aria-hidden", "true");
  host.style.cssText = "position:fixed;inset:0;pointer-events:none;z-index:2147483646;";
  host.attachShadow({ mode: "open" });
  document.documentElement.appendChild(host);
  renderHints("");
}

function pointFor(el) {
  const rect = el.getBoundingClientRect();
  const points = [
    [rect.left + rect.width / 2, rect.top + rect.height / 2],
    [rect.left + 12, rect.top + 12],
    [rect.right - 12, rect.bottom - 12],
  ];
  for (const [x, y] of points) {
    const hit = document.elementFromPoint(x, y);
    if (hit && (hit === el || el.contains(hit))) return { x, y };
  }
  return null;
}

function click(el, reason) {
  const point = pointFor(el);
  clearOverlay();
  state.mode = "normal";
  state.query = "";
  if (reason === "composer" && el.focus) el.focus();
  if (!point) return;
  report({ type: "click", reason, x: point.x, y: point.y });
}

function applyScroll(effect) {
  const ctx = buildCtx();
  const target = selectScrollTarget(ctx, effect.motion);
  const el = target && nodes.get(target.id);
  if (!el) return;
  const pane = paneOf(el);
  if (pane) lastPane = pane;
  const plan = scrollPlan(target, effect.motion, effect.count, matchMedia("(prefers-reduced-motion: reduce)").matches);
  if (plan.method === "scrollTo" || plan.method === "scrollBy") {
    const beforeTop = el.scrollTop;
    const beforeLeft = el.scrollLeft;
    if (plan.method === "scrollTo") el.scrollTo({ top: plan.top, left: plan.left, behavior: plan.behavior });
    else el.scrollBy({ top: plan.top, left: plan.left, behavior: plan.behavior });
    if (plan.method === "scrollBy" && Math.abs(el.scrollTop - beforeTop) <= 1 && Math.abs(el.scrollLeft - beforeLeft) <= 1) {
      const rect = el.getBoundingClientRect();
      report({ type: "wheel", x: rect.left + rect.width / 2, y: rect.top + rect.height / 2, deltaX: plan.left || 0, deltaY: plan.top || 0 });
    }
  } else if (plan.method === "wheel") {
    const rect = el.getBoundingClientRect();
    report({ type: "wheel", x: rect.left + rect.width / 2, y: rect.top + rect.height / 2, deltaX: plan.deltaX, deltaY: plan.deltaY });
  }
}

function apply(effect) {
  if (effect.type === "scroll") applyScroll(effect);
  else if (effect.type === "key") report({ type: "key", keyCode: effect.keyCode, modifiers: effect.modifiers });
  else if (effect.type === "hint-open") collect(effect.scope);
  else if (effect.type === "hint-query") {
    const matched = matchHints(assigned.map((item) => item.hint), effect.query);
    if (matched.activateIndex != null) click(assigned[matched.activateIndex].el, "hint");
    else renderHints(effect.query);
  } else if (effect.type === "hint-activate") {
    const matched = matchHints(assigned.map((item) => item.hint), effect.query || "");
    const item = matched.matches[0] ? assigned[matched.matches[0].index] : null;
    if (item) click(item.el, "hint");
    else clearOverlay();
  } else if (effect.type === "hint-cancel") clearOverlay();
  else if (effect.type === "focus-composer") {
    const target = composerTarget(buildCtx());
    const el = target && nodes.get(target.id);
    if (el) click(el, "composer");
  } else if (effect.type === "blur") {
    const el = document.activeElement;
    if (el && el.blur) el.blur();
    if (editable(document.activeElement)) {
      const pane = paneElement(lastPane) || paneElement("primary");
      const background = pane && pane.querySelector(".c-virtual_list__scroll_container, .p-message_pane");
      if (background) click(background, "blur");
    }
  }
}

function onKey(event) {
  if (!event.isTrusted) return;
  const input = {
    type: event.type,
    key: event.key,
    code: event.code,
    shift: event.shiftKey,
    ctrl: event.ctrlKey,
    alt: event.altKey,
    meta: event.metaKey,
    repeat: event.repeat,
    composing: event.isComposing,
    time: event.timeStamp,
  };
  const ctx = buildCtx();
  const result = reduce(state, input, ctx);
  state = result.state;
  if (result.prevent) {
    event.preventDefault();
    event.stopImmediatePropagation();
  }
  for (const effect of result.effects) apply(effect);
  report({ type: "mode", mode: state.mode, focusNormal: !ctx.editable && !ctx.menu && !ctx.emoji && !ctx.dialog });
}

function cancelLocal() {
  clearOverlay();
  state = initialState();
  report({ type: "mode", mode: "normal", focusNormal: false });
}

if (window.__slackDotfilesKeys) return;
window.__slackDotfilesKeys = true;
window.__slackDotfilesKeysClear = cancelLocal;
window.addEventListener("keydown", onKey, true);
window.addEventListener("keyup", onKey, true);
window.addEventListener("mousedown", () => { state.prefix = ""; state.count = 0; if (state.mode === "hint") cancelLocal(); }, true);
window.addEventListener("blur", cancelLocal);
window.addEventListener("mousemove", (event) => { pointer = overflowAt(event.clientX, event.clientY); }, true);
report({ type: "mode", mode: "normal", focusNormal: !editable(document.activeElement) });
`;

function pageSource(token) {
  const embedded = [initialState, cloneState, swallow, currentPane, selectScrollTarget, scrollPlan, composerTarget, assignHints, matchHints, escAction, orderHintTargets, reduceHint, reduceNormal, reduce]
    .map((fn) => fn.toString())
    .join("\n");
  return `"use strict";\n(() => {\nconst HINT_CHARS = ${JSON.stringify(HINT_CHARS)};\nconst TOKEN = ${JSON.stringify(String(token))};\n${embedded}\n${PAGE}\n})();\n`;
}

let installed = false;

function install() {
  if (process.env.SLACK_DOTFILES_KEYS === "0") return;
  if (installed) return;
  try {
    const { app } = require("electron");
    const allowedURL = loadAllowedURL(require);
    if (!allowedURL) return;
    installed = true;
    const crypto = require("node:crypto");
    app.on("web-contents-created", (_event, wc) => {
      const entry = {
        token: crypto.randomBytes(16).toString("hex"),
        generation: 0,
        hint: false,
        armed: false,
        focusNormal: false,
        warned: false,
      };
      const allowedNow = () => {
        try { return allowedURL(wc.getURL()); } catch { return false; }
      };
      const contentSize = () => {
        try {
          const { BrowserWindow } = require("electron");
          const win = BrowserWindow.fromWebContents(wc);
          if (win) return win.getContentSize();
        } catch {
          // A missing window must not turn a click into an unbounded event.
        }
        return [0, 0];
      };
      const inside = (x, y) => {
        if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || y < 0) return false;
        const [width, height] = contentSize();
        if (!width || !height) return x <= 10000 && y <= 10000;
        return x <= width && y <= height;
      };
      const sendClick = (x, y) => {
        const point = { x: Math.round(x), y: Math.round(y) };
        wc.sendInputEvent({ type: "mouseMove", x: point.x, y: point.y });
        wc.sendInputEvent({ type: "mouseDown", x: point.x, y: point.y, button: "left", clickCount: 1 });
        wc.sendInputEvent({ type: "mouseUp", x: point.x, y: point.y, button: "left", clickCount: 1 });
      };
      wc.on("console-message", (event) => {
        const message = event && event.message;
        if (typeof message !== "string" || !message.startsWith("SLACK_DOTFILES_KEYS\t")) return;
        const parts = message.split("\t");
        if (parts[1] !== entry.token || !allowedNow() || wc.isDestroyed()) return;
        let payload;
        try { payload = JSON.parse(parts[2]); } catch { return; }
        if (!payload || typeof payload.type !== "string") return;
        if (payload.type === "mode") {
          entry.hint = payload.mode === "hint";
          entry.focusNormal = payload.focusNormal === true;
          if (!entry.hint) entry.armed = false;
          return;
        }
        if (payload.type === "key" && payload.keyCode === "G" && Array.isArray(payload.modifiers) && payload.modifiers.length === 1 && payload.modifiers[0] === "control") {
          wc.sendInputEvent({ type: "keyDown", keyCode: "G", modifiers: ["control"] });
          wc.sendInputEvent({ type: "keyUp", keyCode: "G", modifiers: ["control"] });
          return;
        }
        if (payload.type === "click" && (entry.hint || payload.reason === "composer" || payload.reason === "blur")) {
          if (!inside(payload.x, payload.y)) return;
          sendClick(payload.x, payload.y);
          entry.hint = false;
          entry.armed = false;
          return;
        }
        if (payload.type === "wheel" && inside(payload.x, payload.y) && Number.isFinite(payload.deltaX) && Number.isFinite(payload.deltaY)) {
          wc.sendInputEvent({
            type: "mouseWheel",
            x: Math.round(payload.x),
            y: Math.round(payload.y),
            deltaX: payload.deltaX,
            deltaY: payload.deltaY,
            hasPreciseScrollingDeltas: true,
            canScroll: true,
          });
        }
      });
      wc.on("before-input-event", (event, input) => {
        if (!input || input.type !== "keyDown" || !allowedNow()) return;
        if (input.key === "f" && !input.control && !input.alt && !input.meta && !input.shift && entry.focusNormal) entry.armed = true;
        if (input.key === "Escape" && (entry.armed || entry.hint)) {
          event.preventDefault();
          entry.armed = false;
          entry.hint = false;
          wc.executeJavaScript("window.__slackDotfilesKeysClear && window.__slackDotfilesKeysClear()").catch(() => {});
        }
      });
      wc.on("did-start-navigation", (_event, _url, inPlace, mainFrame) => {
        if (mainFrame && !inPlace) {
          entry.generation += 1;
          entry.hint = false;
          entry.armed = false;
        }
      });
      wc.on("dom-ready", () => {
        if (!allowedNow() || wc.isDestroyed()) return;
        const generation = entry.generation;
        wc.executeJavaScript(pageSource(entry.token)).then(() => {
          if (entry.generation !== generation || wc.isDestroyed()) {
            wc.executeJavaScript("window.__slackDotfilesKeysClear && window.__slackDotfilesKeysClear()").catch(() => {});
          }
        }).catch(() => {
          if (!entry.warned) {
            console.warn("[dotfiles-keys] Injection failed");
            entry.warned = true;
          }
        });
      });
    });
  } catch {
    // Missing Electron or a changed main-process API leaves Slack usable.
  }
}

module.exports = {
  HINT_CHARS,
  enabled,
  loadAllowedURL,
  initialState,
  reduce,
  selectScrollTarget,
  scrollPlan,
  currentPane,
  composerTarget,
  assignHints,
  matchHints,
  escAction,
  orderHintTargets,
  pageSource,
  install,
};
