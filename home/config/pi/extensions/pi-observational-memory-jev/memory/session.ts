import { existsSync, readFileSync } from "node:fs";
import { sessionMemoryRoot } from "./paths.js";
import { existingMemoryRoot, seedFromParentMemory } from "./parent-env.js";

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
	return existingMemoryRoot(sessionMemoryRoot(ctx.cwd, parentId));
}

export function ensureSessionMemory(ctx: SessionCtx): string {
	const sessionId = ctx.sessionManager.getSessionId();
	const root = sessionMemoryRoot(ctx.cwd, sessionId);
	if (existsSync(root)) return root;

	const parent = parentMemoryRoot(ctx);
	if (parent) seedFromParentMemory(parent, root);
	return root;
}
