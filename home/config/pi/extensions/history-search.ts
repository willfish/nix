/**
 * history-search.ts — incremental prompt-history search (bash Ctrl+R style).
 *
 * Press Ctrl+R to start a reverse search over every prompt you have ever
 * submitted (scanned from session files under the agent dir and the current
 * project's .pi/sessions). Keep typing to refine the pattern, like bash's
 * reverse-i-search:
 *
 *   - printable keys / backspace: refine the search pattern
 *   - Ctrl+R: older match
 *   - Ctrl+S: newer match
 *   - Enter:  accept the current match (and submit it)
 *   - Esc or Ctrl+C: cancel and restore the previous editor contents
 *
 * While searching, the editor buffer always shows the current match (or the
 * raw pattern when nothing matches), so Enter re-submits an old prompt.
 * Arrow keys still work for moving around a multi-line match.
 */

import {
	CustomEditor,
	getAgentDir,
	type ExtensionAPI,
	type ExtensionContext,
} from "@earendil-works/pi-coding-agent";
import { matchesKey } from "@earendil-works/pi-tui";
import { closeSync, openSync, readSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

const WIDGET_ID = "history-search";
const MAX_ENTRIES = 5000;
const MAX_MATCHES = 200;
const MAX_FILE_BYTES = 4 * 1024 * 1024;

interface SearchState {
	active: boolean;
	pattern: string;
	matches: string[];
	index: number;
	originalBuffer: string;
}

interface HistorySearchSession {
	state: SearchState;
	editor: HistorySearchEditor | null;
}

function collectJsonlFiles(dir: string): { path: string; mtime: number }[] {
	const out: { path: string; mtime: number }[] = [];
	const walk = (d: string, depth: number): void => {
		if (depth > 4) return;
		let entries;
		try {
			entries = readdirSync(d, { withFileTypes: true });
		} catch {
			return;
		}
		for (const entry of entries) {
			const full = join(d, entry.name);
			if (entry.isDirectory()) {
				walk(full, depth + 1);
			} else if (entry.isFile() && entry.name.endsWith(".jsonl")) {
				try {
					out.push({ path: full, mtime: statSync(full).mtimeMs });
				} catch {
					/* file vanished, skip */
				}
			}
		}
	};
	walk(dir, 0);
	return out;
}

function extractUserText(entry: unknown): string | null {
	if (typeof entry !== "object" || entry === null) return null;
	const e = entry as { type?: string; message?: { role?: string; content?: unknown } };
	if (e.type !== "message" || e.message?.role !== "user") return null;
	const content = e.message.content;
	let text: string;
	if (typeof content === "string") {
		text = content;
	} else if (Array.isArray(content)) {
		text = (content as { type?: string; text?: unknown }[])
			.filter((b) => b?.type === "text" && typeof b.text === "string")
			.map((b) => b.text as string)
			.join("\n");
	} else {
		return null;
	}
	text = text.trim();
	if (!text || text.startsWith("/")) return null; // skip commands like /reload
	return text;
}

function readTail(file: string): string {
	try {
		const stat = statSync(file);
		if (stat.size <= MAX_FILE_BYTES) return readFileSync(file, "utf8");
		const fd = openSync(file, "r");
		try {
			const buf = Buffer.alloc(MAX_FILE_BYTES);
			readSync(fd, buf, 0, MAX_FILE_BYTES, stat.size - MAX_FILE_BYTES);
			return buf.toString("utf8");
		} finally {
			closeSync(fd);
		}
	} catch {
		return "";
	}
}

/** Collect submitted user prompts, newest first, de-duplicated. */
export function loadHistory(cwd: string): string[] {
	const dirs = new Set<string>([join(getAgentDir(), "sessions"), join(cwd, ".pi", "sessions")]);
	const files = new Map<string, number>();
	for (const dir of dirs) {
		for (const f of collectJsonlFiles(dir)) {
			if (!files.has(f.path)) files.set(f.path, f.mtime);
		}
	}
	const ordered = [...files.entries()].sort((a, b) => b[1] - a[1]);
	const entries: string[] = [];
	const seen = new Set<string>();
	for (const [path] of ordered) {
		if (entries.length >= MAX_ENTRIES) break;
		for (const line of readTail(path).split("\n")) {
			if (!line.trim()) continue;
			let entry: unknown;
			try {
				entry = JSON.parse(line);
			} catch {
				continue;
			}
			const text = extractUserText(entry);
			if (text && !seen.has(text)) {
				seen.add(text);
				entries.push(text);
				if (entries.length >= MAX_ENTRIES) break;
			}
		}
	}
	return entries;
}

function findMatches(history: string[], pattern: string): string[] {
	if (!pattern) return history.slice(0, MAX_MATCHES);
	const needle = pattern.toLowerCase();
	return history.filter((h) => h.toLowerCase().includes(needle)).slice(0, MAX_MATCHES);
}

class HistorySearchEditor extends CustomEditor {
	private session: HistorySearchSession;
	private ctx: ExtensionContext | null = null;
	private history: string[] = [];

	constructor(tui: unknown, theme: unknown, keybindings: unknown, session: HistorySearchSession) {
		super(tui, theme, keybindings);
		this.session = session;
		session.editor = this;
	}

	/** The extension context is attached lazily (it is only known at session_start). */
	bindContext(ctx: ExtensionContext): void {
		this.ctx = ctx;
	}

	get state(): SearchState {
		return this.session.state;
	}

	private refreshHistory(): void {
		if (this.history.length === 0 && this.ctx) this.history = loadHistory(this.ctx.cwd);
	}

	private updateWidget(): void {
		if (!this.ctx?.hasUI) return;
		const s = this.state;
		try {
			if (!s.active) {
				this.ctx.ui.setWidget(WIDGET_ID, undefined);
				return;
			}
			const pos = s.matches.length ? `${s.index + 1}/${s.matches.length}` : "0/0";
			const label = s.matches.length
				? `history ${pos}  "${s.pattern}"`
				: `history: no match for "${s.pattern}"`;
			this.ctx.ui.setWidget(WIDGET_ID, [
				`${label}   enter:accept  ctrl+r:older  ctrl+s:newer  esc:cancel`,
			]);
		} catch {
			/* widget is best-effort */
		}
	}

	private showCurrent(): void {
		const s = this.state;
		this.setText(s.matches[s.index] ?? s.pattern);
		this.updateWidget();
	}

	private refreshMatches(): void {
		const s = this.state;
		s.matches = findMatches(this.history, s.pattern);
		s.index = 0;
		this.showCurrent();
	}

	/** Start a new search seeded with the current buffer contents. */
	start(): void {
		const s = this.state;
		s.active = true;
		s.originalBuffer = this.getText();
		s.pattern = s.originalBuffer;
		this.refreshHistory();
		this.refreshMatches();
	}

	/** Cycle matches. direction: -1 = older, +1 = newer. */
	step(direction: -1 | 1): void {
		const s = this.state;
		if (s.matches.length === 0) {
			this.showCurrent();
			return;
		}
		if (direction === -1) s.index = Math.min(s.index + 1, s.matches.length - 1);
		else s.index = Math.max(s.index - 1, 0);
		this.showCurrent();
	}

	cancel(): void {
		const s = this.state;
		s.active = false;
		this.setText(s.originalBuffer);
		this.updateWidget();
	}

	handleInput(data: string): void {
		const s = this.state;
		if (!s.active) {
			super.handleInput(data);
			return;
		}

		// Search controls (also handled by the registered shortcuts when the
		// editor is inactive; while active we must consume them here so the
		// base class never sees them).
		if (matchesKey(data, "ctrl+r")) {
			this.step(-1);
			return;
		}
		if (matchesKey(data, "ctrl+s")) {
			this.step(1);
			return;
		}
		if (matchesKey(data, "escape") || matchesKey(data, "ctrl+c")) {
			this.cancel();
			return;
		}
		if (data === "\r" || data === "\n") {
			// Accept: keep the buffer (match or pattern) and submit it.
			s.active = false;
			this.updateWidget();
			super.handleInput(data);
			return;
		}

		// Pattern editing
		if (data === "\x7f" || data === "\x08" || data === "\x1b[3~") {
			if (s.pattern.length > 0) {
				s.pattern = s.pattern.slice(0, -1);
				this.refreshMatches();
			}
			return;
		}
		if (data.length > 0 && !data.startsWith("\x1b") && data.charCodeAt(0) >= 32) {
			s.pattern += data;
			this.refreshMatches();
			return;
		}

		// Cursor movement on the (possibly multi-line) match: pass through.
		// Anything else (ctrl+u, tab, ...) is consumed: the buffer is display-only.
		if (data.startsWith("\x1b[")) {
			super.handleInput(data);
		}
	}
}

interface HistorySearchFactory {
	(tui: unknown, theme: unknown, keybindings: unknown): HistorySearchEditor;
	session?: HistorySearchSession;
}

export default function (pi: ExtensionAPI): void {
	let current: HistorySearchSession | null = null;

	pi.registerShortcut("ctrl+r", {
		description: "Search prompt history (bash Ctrl+R style; Ctrl+R older / Ctrl+S newer while searching)",
		handler: (ctx) => {
			const session = current;
			if (!ctx.hasUI || !session?.editor) return;
			if (session.state.active) session.editor.step(-1);
			else session.editor.start();
		},
	});
	pi.registerShortcut("ctrl+s", {
		description: "Search prompt history (newer match while searching)",
		handler: (ctx) => {
			const session = current;
			if (!ctx.hasUI || !session?.editor) return;
			if (session.state.active) session.editor.step(1);
			else session.editor.start();
		},
	});

	pi.on("session_start", (_event, ctx) => {
		// Reuse the previous session on /reload so state and the live editor
		// survive, and avoid double-wrapping the editor component.
		const existing = ctx.ui.getEditorComponent?.() as HistorySearchFactory | undefined;
		const session =
			existing?.session ??
			({
				state: { active: false, pattern: "", matches: [], index: 0, originalBuffer: "" },
				editor: null,
			} satisfies HistorySearchSession);
		current = session;
		const factory = ((tui: unknown, theme: unknown, keybindings: unknown) => {
			const editor = new HistorySearchEditor(tui, theme, keybindings, session);
			editor.bindContext(ctx);
			return editor;
		}) as HistorySearchFactory;
		factory.session = session;
		ctx.ui.setEditorComponent(factory as never);
	});
}
