import { cpSync, existsSync, readFileSync, renameSync, rmSync } from "node:fs";
import { basename, sep } from "node:path";
import { sessionMemoryRoot } from "./paths.js";

type SessionCtx = {
	cwd: string;
	sessionManager: {
		getSessionId: () => string;
		getHeader?: () => { id?: string; cwd?: string; parentSession?: string } | null | undefined;
	};
};

function readSessionHeaderId(file: string): string | undefined {
	try {
		const firstLine = readFileSync(file, "utf-8").split("\n", 1)[0] ?? "";
		const header = JSON.parse(firstLine) as { type?: string; id?: string } | undefined;
		return typeof header?.id === "string" ? header.id : undefined;
	} catch {
		return undefined;
	}
}

function parentMemoryRoot(ctx: SessionCtx): string | undefined {
	const parentFile = ctx.sessionManager.getHeader?.()?.parentSession;
	if (!parentFile) return undefined;
	const parentId = readSessionHeaderId(parentFile);
	if (!parentId) return undefined;
	const root = sessionMemoryRoot(ctx.cwd, parentId);
	return existsSync(root) ? root : undefined;
}

function isRunsPath(p: string): boolean {
	return basename(p) === ".runs" || p.includes(`${sep}.runs${sep}`);
}

export function ensureSessionMemory(ctx: SessionCtx): string {
	const sessionId = ctx.sessionManager.getSessionId();
	const root = sessionMemoryRoot(ctx.cwd, sessionId);
	if (existsSync(root)) return root;

	const parent = parentMemoryRoot(ctx);
	if (parent) {
		const tmp = `${root}.seed-tmp-${process.pid}-${Date.now()}`;
		try {
			cpSync(parent, tmp, { recursive: true, filter: (src) => !isRunsPath(src) });
			renameSync(tmp, root);
		} catch {
			try {
				rmSync(tmp, { recursive: true, force: true });
			} catch {
				/* best-effort cleanup */
			}
		}
	}
	return root;
}
