"use strict";
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

// Consume the same atomically published roles as the Slack integration.
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
  for (const name of ["font", "mono_font"]) {
    // Font names are data, never arbitrary CSS or JavaScript.
    if (!/^[\w .,+-]{1,100}$/.test(values[name] || "")) throw new Error("Invalid theme font");
    theme[name] = values[name];
  }
  return theme;
}

function contrast(a, b) {
  const luminance = (hex) => [1, 3, 5].map((i) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  }).reduce((sum, c, i) => sum + c * [0.2126, 0.7152, 0.0722][i], 0);
  const x = luminance(a), y = luminance(b);
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

function readableText(background, ...candidates) {
  return [...candidates, "#ffffff", "#000000"].find((c) => contrast(c, background) >= 4.5);
}

function mix(a, b, weight) {
  return "#" + [1, 3, 5].map((i) => Math.round(
    parseInt(a.slice(i, i + 2), 16) * (1 - weight) + parseInt(b.slice(i, i + 2), 16) * weight,
  ).toString(16).padStart(2, "0")).join("");
}

function renderTheme(t) {
  const selectedText = readableText(t.selection, t.accent, t.text, t.background);
  const onAccent = readableText(t.accent, t.background, t.text);
  // Terminal palettes can use very low-contrast muted colours. Keep labels readable
  // on both the chat background and sidebar without changing the source palette.
  const muted = [t.muted, ...[0.55, 0.65, 0.75, 0.85, 1].map((w) => mix(t.background, t.text, w))]
    .find((c) => contrast(c, t.background) >= 4.5 && contrast(c, t.surface) >= 4.5) || t.text;
  const tokens = {
    "app-frame-background": t.surface,
    "background-primary": t.background,
    "background-secondary": t.surface,
    "background-secondary-alt": t.surface,
    "background-tertiary": t.surface,
    "background-floating": t.surface,
    "background-accent": t.selection,
    "background-base-lowest": t.surface,
    "background-base-lower": t.surface,
    "background-base-low": t.background,
    "background-surface-high": t.background,
    "background-surface-higher": t.surface,
    "background-surface-highest": t.selection,
    "bg-surface-raised": t.surface,
    "home-background": t.background,
    "chat-background": t.background,
    "chat-background-default": t.background,
    "chat-border": t.border,
    "__header-bar-background": t.surface,
    "modal-background": t.surface,
    "modal-footer-background": t.surface,
    "card-background-default": t.surface,
    "custom-channel-members-bg": t.surface,
    "channeltextarea-background": t.surface,
    "input-background": t.surface,
    "input-background-default": t.surface,
    "input-border-default": t.border,
    "input-text-default": t.text,
    "input-placeholder-text-default": muted,
    "background-code": t.surface,
    "text-normal": t.text,
    "text-default": t.text,
    "text-strong": t.text,
    "text-subtle": muted,
    "text-muted": muted,
    "header-primary": t.text,
    "header-secondary": muted,
    "channels-default": muted,
    "chat-text-muted": muted,
    "icon-default": t.text,
    "icon-strong": t.text,
    "icon-subtle": muted,
    "icon-muted": muted,
    "interactive-normal": t.text,
    "interactive-hover": t.text,
    "interactive-active": selectedText,
    "interactive-muted": muted,
    "interactive-text-default": t.text,
    "interactive-text-hover": t.text,
    "interactive-text-active": selectedText,
    "interactive-icon-default": t.text,
    "interactive-icon-hover": t.text,
    "interactive-icon-active": selectedText,
    "interactive-background-hover": t.selection,
    "interactive-background-selected": t.selection,
    "interactive-background-active": t.selection,
    "background-modifier-hover": t.selection,
    "background-modifier-selected": t.selection,
    "background-mod-subtle": t.selection,
    "background-mod-normal": t.selection,
    "border-subtle": t.border,
    "border-normal": t.border,
    "border-strong": t.border,
    "border-muted": t.border,
    "brand-500": t.accent,
    "brand-560": t.accent,
    "text-link": t.accent,
    "text-brand": t.accent,
    "control-brand-foreground": t.accent,
    "control-primary-background-default": t.accent,
    "control-primary-background-hover": mix(t.accent, t.background, 0.1),
    "control-primary-background-active": mix(t.accent, t.background, 0.2),
    "control-primary-text-default": onAccent,
    "control-primary-text-hover": readableText(mix(t.accent, t.background, 0.1), onAccent),
    "control-primary-text-active": readableText(mix(t.accent, t.background, 0.2), onAccent),
    "control-secondary-background-default": t.surface,
    "control-secondary-background-hover": t.selection,
    "control-secondary-background-active": t.selection,
    "control-secondary-border-default": t.border,
    "control-secondary-text-default": t.text,
    "control-secondary-text-hover": selectedText,
    "control-secondary-text-active": selectedText,
    "mention-foreground": t.accent,
    "mention-background": `${t.accent}26`,
    "message-mentioned-background-default": `${t.accent}26`,
    "message-mentioned-background-hover": `${t.accent}33`,
    "message-background-hover": `${t.selection}80`,
    "message-reacted-background-default": t.selection,
    "message-reacted-text-default": selectedText,
    "scrollbar-thin-thumb": t.border,
    "scrollbar-thin-track": "transparent",
    "scrollbar-auto-thumb": t.border,
    "scrollbar-auto-track": t.surface,
    "__adaptive-focus-ring-color": t.accent,
  };
  return `
:root, .theme-light, .theme-dark, .theme-darker, .theme-midnight {
  ${Object.entries(tokens).map(([k, v]) => `--${k}: ${v} !important;`).join("\n  ")}
  --font-primary: "${t.font}", sans-serif !important;
  --font-display: "${t.font}", sans-serif !important;
  --font-code: "${t.mono_font}", monospace !important;
  color-scheme: ${t.mode} !important;
}
::selection { background-color: ${t.selection}; color: ${selectedText}; }
`;
}

function allowedURL(value) {
  try {
    const url = new URL(value);
    // Stable's bootstrap can still start on the legacy domain before migration.
    return ["https://discord.com", "https://discordapp.com"].includes(url.origin);
  } catch {
    return false;
  }
}

function install() {
  if (process.env.DISCORD_DOTFILES_THEME === "0") return;
  const { app } = require("electron");
  const state = process.env.XDG_STATE_HOME || path.join(os.homedir(), ".local/state");
  const themeFile = path.join(state, "theme-menu/active/hyprland.conf");
  const contents = new Map();
  let css = "", lastSource = null, warned = false;

  function refreshTheme() {
    try {
      const source = fs.readFileSync(themeFile, "utf8");
      if (source !== lastSource) {
        css = renderTheme(parseTheme(source));
        lastSource = source;
      }
      warned = false;
    } catch (error) {
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
    const generation = entry.generation, next = css;
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
  // Poll the stable pathname: theme-menu's atomic rename replaces the inode.
  const timer = setInterval(() => {
    refreshTheme();
    for (const [wc, entry] of contents) void apply(wc, entry);
  }, 2000);
  timer.unref();
  app.once("will-quit", () => clearInterval(timer));
}

module.exports = { parseTheme, renderTheme, contrast, allowedURL, install };
