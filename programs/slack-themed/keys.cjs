"use strict";

// Vimium-shaped keys for the desktop client. / is workspace search, not
// find-in-page: Slack already has an index, and Ctrl+/ must stay help.
// gg/G move the loaded pane, not the channel's full history.
// hjkl/du use the innermost axis-scrollable ancestor under the pointer, then focus.

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
  if (input.code && !state.swallowed.includes(input.code)) state.swallowed.push(input.code);
  return { state, prevent: true, effects };
}

function currentPane(ctx) {
  if (!ctx) return null;
  const alive = (name) => {
    if (name !== "primary" && name !== "secondary") return false;
    if (!ctx.panes) return true;
    return Boolean(ctx.panes[name]);
  };
  if (alive(ctx.focusPane)) return ctx.focusPane;
  if (alive(ctx.lastPane)) return ctx.lastPane;
  if (ctx.panes && ctx.panes.primary) return "primary";
  return null;
}

function liveTarget(target) {
  return target && !target.detached ? target : null;
}

function selectScrollTarget(ctx, motion) {
  const pane = currentPane(ctx);
  const paneTarget = pane && ctx && ctx.panes ? ctx.panes[pane] : null;
  if (motion === "gg" || motion === "G" || motion === "gi") return paneTarget || null;
  if (!ctx) return paneTarget || null;
  const axis = motion === "h" || motion === "l" ? "x" : "y";
  const hovered = liveTarget(ctx.pointerScrollers && ctx.pointerScrollers[axis]) || liveTarget(ctx.pointerScroller);
  if (hovered) return hovered;
  const focused = liveTarget(ctx.activeScrollers && ctx.activeScrollers[axis]) || liveTarget(ctx.activeScroller);
  if (focused) return focused;
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

function needsWheelFallback(plan, before, after) {
  if (!plan || plan.method !== "scrollBy") return false;
  if (plan.behavior === "smooth") return false;
  const startTop = before && Number.isFinite(before.top) ? before.top : 0;
  const startLeft = before && Number.isFinite(before.left) ? before.left : 0;
  const endTop = after && Number.isFinite(after.top) ? after.top : 0;
  const endLeft = after && Number.isFinite(after.left) ? after.left : 0;
  return Math.abs(endTop - startTop) <= 1 && Math.abs(endLeft - startLeft) <= 1;
}

function composerTarget(ctx) {
  const pane = currentPane(ctx);
  if (!pane || !ctx || !ctx.composers) return null;
  const list = ctx.composers.filter((item) => item && item.pane === pane && item.visible && !item.search && item.message && (item.ql || item.textbox || item.textarea));
  if (list.length !== 1) return null;
  return list[0];
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
    if (!input.code || !next.swallowed.includes(input.code)) return { state: next, prevent: false, effects: [] };
    next.swallowed = next.swallowed.filter((code) => code !== input.code);
    return { state: next, prevent: true, effects: [] };
  }
  if (input.composing || input.ctrl || input.alt || input.meta) {
    const effects = [];
    if (next.mode === "hint") effects.push({ type: "hint-cancel" });
    next.mode = "normal";
    next.query = "";
    next.prefix = "";
    next.count = 0;
    next.swallowed = [];
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
let pointerEl = null;
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

function rootHost(node) {
  const root = node && node.getRootNode ? node.getRootNode() : null;
  return root && root.nodeType === 11 && root.host ? root.host : null;
}

function ancestor(node) {
  if (!node) return null;
  if (node.parentElement) return node.parentElement;
  return rootHost(node);
}

function nodeInside(root, node) {
  let current = node;
  while (current) {
    if (current === root) return true;
    current = ancestor(current);
  }
  return false;
}

function paneOf(el) {
  let node = el;
  while (node && node.nodeType === 1) {
    if (node.closest(".p-workspace__secondary_view, .p-threads_view")) return "secondary";
    if (node.closest(".p-workspace__primary_view, .p-message_pane")) return "primary";
    node = rootHost(node);
  }
  return null;
}

function rememberPane(el) {
  const pane = paneOf(el);
  if (pane) lastPane = pane;
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

function canScroll(el, axis) {
  if (!el || el.nodeType !== 1 || !el.isConnected) return false;
  if (el === document.body || el === document.documentElement) return false;
  if (el.hasAttribute("data-dotfiles-hints")) return false;
  const style = getComputedStyle(el);
  const overflow = axis === "x" ? style.overflowX : style.overflowY;
  if (overflow !== "auto" && overflow !== "scroll" && overflow !== "overlay") return false;
  if (axis === "x") return el.scrollWidth > el.clientWidth + 1;
  return el.scrollHeight > el.clientHeight + 1;
}

function scrollerFrom(start, axis) {
  if (!start || start.nodeType !== 1 || !start.isConnected) return null;
  let node = start;
  while (node && node !== document.body && node !== document.documentElement) {
    if (canScroll(node, axis)) return node;
    node = ancestor(node);
  }
  return null;
}

function deepElementFromPoint(x, y) {
  let hit = document.elementFromPoint(x, y);
  const seen = new Set();
  while (hit && hit.shadowRoot && !seen.has(hit)) {
    seen.add(hit);
    let inner = null;
    try { inner = hit.shadowRoot.elementFromPoint(x, y); } catch { inner = null; }
    if (!inner || inner === hit) break;
    hit = inner;
  }
  return hit;
}

function interactive(el) {
  if (!el || el.nodeType !== 1) return false;
  const tagName = el.tagName;
  if (tagName === "A" && el.hasAttribute("href")) return true;
  if (tagName === "BUTTON" || tagName === "SUMMARY" || tagName === "LABEL" || tagName === "TEXTAREA" || tagName === "SELECT") return true;
  if (tagName === "INPUT") return true;
  if (el.isContentEditable) return true;
  const role = el.getAttribute("role");
  return ["button", "link", "menuitem", "tab", "option", "checkbox", "switch", "textbox", "searchbox"].includes(role);
}

function interactiveBetween(root, hit) {
  let node = hit;
  while (node && node !== root) {
    if (interactive(node)) return true;
    node = ancestor(node);
  }
  return false;
}

function ownEditingHost(el) {
  if (!el || el.nodeType !== 1) return false;
  if (el.classList.contains("ql-editor")) return true;
  if (el.contentEditable === "true") return true;
  const role = el.getAttribute("role");
  return role === "textbox" || role === "searchbox";
}

function hardControl(el) {
  if (!el || el.nodeType !== 1) return false;
  const tagName = el.tagName;
  if (tagName === "A" && el.hasAttribute("href")) return true;
  if (tagName === "BUTTON" || tagName === "SUMMARY" || tagName === "LABEL" || tagName === "TEXTAREA" || tagName === "SELECT") return true;
  if (tagName === "INPUT") return true;
  const role = el.getAttribute("role");
  return ["button", "link", "menuitem", "tab", "option", "checkbox", "switch"].includes(role);
}

function nestedAction(root, hit) {
  let node = hit;
  while (node && node !== root) {
    if (hardControl(node) || ownEditingHost(node)) return true;
    node = ancestor(node);
  }
  return false;
}

function insideEditor(node) {
  let current = node;
  while (current && current.nodeType === 1) {
    if (current.isContentEditable || ownEditingHost(current)) return true;
    current = ancestor(current);
  }
  return false;
}

function buildCtx() {
  const focus = document.activeElement;
  const dialog = openDialog();
  const primary = paneElement("primary");
  const secondary = paneElement("secondary");
  const composers = [];
  for (const el of document.querySelectorAll(".ql-editor, [role=textbox], textarea")) {
    const search = Boolean(el.closest(".p-top_nav, [role=search]")) || el.getAttribute("type") === "search";
    const inComposer = Boolean(el.closest(".p-message_input, .p-message_input_field"));
    const inMessage = Boolean(el.closest(".c-message_kit__background, .c-message_kit__text"));
    const message = inComposer && !inMessage;
    composers.push({
      id: tag(el),
      pane: paneOf(el) || "primary",
      visible: visible(el),
      search,
      message,
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
    pointerScroller: null,
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
  let node = el;
  while (node && node.nodeType === 1) {
    if (node.closest(".p-channel_sidebar, .p-tab_rail, .p-team_sidebar")) return "sidebar";
    node = rootHost(node);
  }
  if (pane && nodeInside(pane, el)) return "current";
  return "other";
}

function overlapRatio(outer, inner) {
  const a = outer.getBoundingClientRect();
  const b = inner.getBoundingClientRect();
  const width = Math.max(0, Math.min(a.right, b.right) - Math.max(a.left, b.left));
  const height = Math.max(0, Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top));
  const area = a.width * a.height;
  if (!(area > 0)) return 0;
  return (width * height) / area;
}

function collect(scope) {
  const dialog = scope === "dialog" ? openDialog() : null;
  const root = dialog || document.body;
  const paneName = currentPane({
    focusPane: paneOf(document.activeElement),
    lastPane,
    panes: { primary: Boolean(paneElement("primary")), secondary: Boolean(paneElement("secondary")) },
  });
  const pane = paneElement(paneName) || paneElement("primary");
  const found = [];
  const walk = (node) => {
    if (!node) return;
    if (node.nodeType === 11) {
      for (const child of node.children || []) walk(child);
      return;
    }
    if (node.nodeType !== 1) return;
    if (hintable(node)) found.push(node);
    if (node.shadowRoot) walk(node.shadowRoot);
    for (const child of node.children || []) walk(child);
  };
  walk(root);
  const hittable = found.filter((el) => pointFor(el));
  const pruned = hittable.filter((el) => !hittable.some((other) => other !== el && nodeInside(el, other) && overlapRatio(el, other) >= 0.8));
  const targets = pruned.map((el, index) => ({ el, group: groupOf(el, pane), index }));
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
  if (rect.width < 4 || rect.height < 4) return null;
  const insetX = Math.min(12, rect.width / 2);
  const insetY = Math.min(12, rect.height / 2);
  const points = [
    [rect.left + rect.width / 2, rect.top + rect.height / 2],
    [rect.left + insetX, rect.top + insetY],
    [rect.right - insetX, rect.bottom - insetY],
  ];
  for (const [x, y] of points) {
    if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) continue;
    const hit = deepElementFromPoint(x, y);
    if (!hit || !nodeInside(el, hit)) continue;
    if (hit !== el && nestedAction(el, hit)) continue;
    return { x, y };
  }
  return null;
}

function backgroundPoint(root) {
  if (!root || !root.getBoundingClientRect) return null;
  const rect = root.getBoundingClientRect();
  if (rect.width < 8 || rect.height < 8) return null;
  for (let row = 1; row <= 4; row += 1) {
    for (let col = 1; col <= 4; col += 1) {
      const x = rect.left + (rect.width * col) / 5;
      const y = rect.top + (rect.height * row) / 5;
      if (x < 1 || y < 1 || x >= innerWidth - 1 || y >= innerHeight - 1) continue;
      const hit = deepElementFromPoint(x, y);
      if (!hit || !nodeInside(root, hit) || insideEditor(hit) || interactive(hit) || interactiveBetween(root, hit)) continue;
      return { x, y };
    }
  }
  return null;
}

function click(el, reason) {
  if (reason === "hint" || reason === "composer") rememberPane(el);
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
  const axis = effect.motion === "h" || effect.motion === "l" ? "x" : "y";
  const hovered = scrollerFrom(pointerEl, axis);
  const focused = scrollerFrom(document.activeElement, axis);
  ctx.pointerScrollers = {};
  ctx.activeScrollers = {};
  ctx.pointerScrollers[axis] = hovered ? dims(hovered) : null;
  ctx.activeScrollers[axis] = focused ? dims(focused) : null;
  ctx.pointerScroller = null;
  ctx.activeScroller = null;
  let target = selectScrollTarget(ctx, effect.motion);
  let el = target && nodes.get(target.id);
  if (!el || !el.isConnected) {
    ctx.pointerScrollers = null;
    ctx.activeScrollers = null;
    target = selectScrollTarget(ctx, effect.motion);
    el = target && nodes.get(target.id);
  }
  if (!el || !el.isConnected) return;
  const pane = paneOf(el);
  if (pane) lastPane = pane;
  const plan = scrollPlan(target, effect.motion, effect.count, matchMedia("(prefers-reduced-motion: reduce)").matches);
  if (plan.method === "scrollTo" || plan.method === "scrollBy") {
    const before = { top: el.scrollTop, left: el.scrollLeft };
    if (plan.method === "scrollTo") el.scrollTo({ top: plan.top, left: plan.left, behavior: plan.behavior });
    else el.scrollBy({ top: plan.top, left: plan.left, behavior: plan.behavior });
    if (needsWheelFallback(plan, before, { top: el.scrollTop, left: el.scrollLeft })) {
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
    rememberPane(el);
    if (el && el.blur) el.blur();
    if (!editable(document.activeElement)) return;
    const pane = paneElement(paneOf(el) || lastPane) || paneElement("primary");
    const point = backgroundPoint(pane);
    if (!point) return;
    report({ type: "click", reason: "blur", x: point.x, y: point.y });
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
window.addEventListener("focusin", (event) => {
  if (editable(event.target)) rememberPane(event.target);
}, true);
window.addEventListener("mousemove", (event) => {
  pointerEl = deepElementFromPoint(event.clientX, event.clientY);
}, true);
report({ type: "mode", mode: "normal", focusNormal: !editable(document.activeElement) });
`;

function pageSource(token) {
  const embedded = [initialState, cloneState, swallow, currentPane, liveTarget, selectScrollTarget, scrollPlan, needsWheelFallback, composerTarget, assignHints, matchHints, escAction, orderHintTargets, reduceHint, reduceNormal, reduce]
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
  needsWheelFallback,
  currentPane,
  composerTarget,
  assignHints,
  matchHints,
  escAction,
  orderHintTargets,
  pageSource,
  install,
};
