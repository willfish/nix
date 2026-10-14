"use strict";
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

// Read one atomically published theme-menu asset, not Slack's profile or data.
function parseTheme(text) {
  const values = Object.fromEntries(
    [...text.matchAll(/^\$theme_(\w+) = (.+)$/gm)].map((m) => [m[1], m[2].trim()]),
  );
  const theme = {};
  for (const name of ["background", "surface", "selection", "border", "muted", "text", "accent", "fail", "check"]) {
    const match = /^rgb\(([0-9a-fA-F]{6})\)$/.exec(values[name] || "");
    if (!match) throw new Error("Invalid theme colour");
    theme[name] = `#${match[1]}`;
  }
  if (!["dark", "light"].includes(values.mode)) throw new Error("Invalid theme mode");
  theme.mode = values.mode;
  // Font names are data, never arbitrary CSS or JavaScript.
  for (const name of ["font", "mono_font"]) {
    if (!/^[\w .,+-]{1,100}$/.test(values[name] || "")) throw new Error("Invalid theme font");
    theme[name] = values[name];
  }
  return theme;
}

function selectionText(t) {
  const luminance = (hex) => [1, 3, 5].map((i) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  }).reduce((sum, c, i) => sum + c * [0.2126, 0.7152, 0.0722][i], 0);
  const background = luminance(t.selection);
  const contrast = (hex) => {
    const foreground = luminance(hex);
    return (Math.max(foreground, background) + 0.05) / (Math.min(foreground, background) + 0.05);
  };
  return [t.accent, t.text, t.background, "#ffffff", "#000000"].find((c) => contrast(c) >= 4.5);
}

function renderTheme(t) {
  const selectedText = selectionText(t);
  const rgb = (hex) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)).join(",");
  // Slack's sk tokens are comma-separated RGB channels, not hex colours.
  const tokens = {
    primary_background: t.background,
    secondary_background: t.surface,
    primary_foreground: t.text,
    secondary_foreground: t.muted,
    tertiary_foreground: t.muted,
    primary_border: t.border,
    secondary_border: t.border,
    highlight: t.accent,
    highlight_accent: t.accent,
  };
  return `
:root, body, .sk-client-theme--dark, .sk-client-theme--light {
  ${Object.entries(tokens).map(([k, v]) => `--sk_${k}: ${rgb(v)} !important;`).join("\n  ")}
  --p-channel_sidebar__column-bg: ${t.surface} !important;
  --p-channel_sidebar__text-color: ${t.text} !important;
  --p-channel_sidebar__heading-color: ${t.text} !important;
  --p-channel_sidebar__muted-text-color: ${t.muted} !important;
  --p-channel_sidebar__hover-item-bg: ${t.background} !important;
  --p-channel_sidebar__active-item-bg: ${t.selection} !important;
  --p-channel_sidebar__active-item-text-color: ${selectedText} !important;
  --p-channel_sidebar__active-presence: ${t.check} !important;
  --p-channel_sidebar__badge-color: ${t.fail} !important;
  color-scheme: ${t.mode} !important;
}
body, .p-client_workspace, .p-workspace, .p-workspace__primary_view,
.p-workspace__secondary_view, .p-view_contents, .c-virtual_list__scroll_container,
.c-message_kit__background, .p-message_pane, .p-threads_view,
.p-channel_details, .p-channel_details__body {
  background-color: ${t.background} !important;
  color: ${t.text} !important;
}
body, .p-client_workspace, button, input, textarea, select, .ql-editor {
  font-family: "${t.font}", sans-serif !important;
}
pre, code, .c-mrkdwn__pre, .c-mrkdwn__code {
  font-family: "${t.mono_font}", monospace !important;
  background-color: ${t.surface} !important;
  color: ${t.text} !important;
}
.p-client_workspace__layout, .p-client_workspace__layout::before,
.p-client_workspace__layout::after, .p-tab_rail, .p-team_sidebar,
.p-channel_sidebar, .p-channel_sidebar__channel, .p-channel_sidebar__section_heading,
.p-top_nav, .p-workspace__top_nav, .p-workspace__sidebar,
.p-view_header, .p-channel_header, .p-ia__view_header,
.c-dialog, .c-dialog__header, .c-dialog__footer, .c-menu,
.p-message_input, .p-message_input_field, .c-texty_input,
.ql-editor, .c-input_text, .c-input_textarea {
  background-color: ${t.surface} !important;
  color: ${t.text} !important;
  border-color: ${t.border} !important;
}
.p-channel_sidebar__link, .p-channel_sidebar__name, .c-message__sender,
.c-message_kit__sender, .p-view_header__text, .p-channel_header__name {
  color: ${t.text} !important;
}
.p-channel_sidebar__channel:hover, .p-channel_sidebar__channel:hover .p-channel_sidebar__link,
.p-channel_sidebar__channel:hover .p-channel_sidebar__name,
.p-channel_sidebar__channel--selected, .p-channel_sidebar__channel--selected .p-channel_sidebar__link,
.p-channel_sidebar__channel--selected .p-channel_sidebar__name {
  background-color: ${t.selection} !important;
  color: ${selectedText} !important;
}
a, .c-link, .c-message_kit__text a, .p-top_nav__search {
  color: ${t.accent} !important;
}
.c-timestamp, .c-message_kit__timestamp, .c-message__edited_label,
.p-channel_sidebar__section_heading_label, .ql-editor.ql-blank::before {
  color: ${t.muted} !important;
}
:focus-visible {
  outline: 2px solid ${t.accent} !important;
  outline-offset: 2px !important;
}
::selection { background-color: ${t.selection}; color: ${selectedText}; }
`;
}

function allowedURL(value) {
  try {
    const url = new URL(value);
    return url.protocol === "app:" ||
      (url.protocol === "https:" &&
        (url.hostname === "slack.com" || url.hostname.endsWith(".slack.com")));
  } catch {
    return false;
  }
}

function install() {
  if (process.env.SLACK_DOTFILES_THEME === "0") return;
  const { app } = require("electron");
  const state = process.env.XDG_STATE_HOME || path.join(os.homedir(), ".local/state");
  const themeFile = path.join(state, "theme-menu/active/hyprland.conf");
  const contents = new Map();
  let css = "";
  let lastSource = null;
  let warned = false;

  function refreshTheme() {
    try {
      const source = fs.readFileSync(themeFile, "utf8");
      if (source !== lastSource) {
        css = renderTheme(parseTheme(source));
        lastSource = source;
      }
      warned = false;
    } catch (error) {
      // Missing/invalid configuration leaves Slack usable with its last good theme.
      if (error.code !== "ENOENT" && !warned) {
        console.warn("[dotfiles-theme] Cannot read theme; keeping the last valid style");
        warned = true;
      }
    }
  }

  async function apply(wc, entry) {
    if (entry.busy || !entry.ready || !css || entry.css === css || wc.isDestroyed()) return;
    if (!allowedURL(wc.getURL())) return;
    entry.busy = true;
    const generation = entry.generation;
    const next = css;
    try {
      const key = await wc.insertCSS(next, { cssOrigin: "user" });
      if (wc.isDestroyed()) return;
      if (entry.generation !== generation) {
        await wc.removeInsertedCSS(key);
        return;
      }
      const previous = entry.key;
      entry.key = key;
      entry.css = next;
      if (previous) await wc.removeInsertedCSS(previous);
    } catch {
      // Navigation/destruction can cancel insertion; retry on the next tick.
    } finally {
      entry.busy = false;
    }
  }

  refreshTheme();
  app.on("web-contents-created", (_event, wc) => {
    const entry = { ready: false, generation: 0, css: "", key: null, busy: false };
    contents.set(wc, entry);
    wc.on("did-start-navigation", (_event, _url, inPlace, mainFrame) => {
      if (mainFrame && !inPlace) {
        entry.generation++;
        entry.ready = false;
        entry.css = "";
        entry.key = null;
      }
    });
    wc.on("dom-ready", () => {
      entry.ready = true;
      void apply(wc, entry);
    });
    wc.once("destroyed", () => contents.delete(wc));
  });
  const timer = setInterval(() => {
    refreshTheme();
    for (const [wc, entry] of contents) void apply(wc, entry);
  }, 2000);
  timer.unref();
  app.once("will-quit", () => clearInterval(timer));
}

module.exports = { parseTheme, renderTheme, selectionText, allowedURL, install };
