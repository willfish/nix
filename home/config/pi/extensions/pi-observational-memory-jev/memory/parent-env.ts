import { cpSync, existsSync, renameSync, rmSync } from "node:fs";
import { basename, dirname, resolve, sep } from "node:path";

export function existingMemoryRoot(root: string | undefined): string | undefined {
	return root && existsSync(root) ? root : undefined;
}

/** Team children pass the coordinator `.memory/<sessionId>` root. */
export function envParentMemoryRoot(env: NodeJS.ProcessEnv = process.env): string | undefined {
	const raw = env.PI_OM_PARENT_MEMORY?.trim();
	if (!raw) return undefined;
	const abs = resolve(raw);
	if (basename(dirname(abs)) !== ".memory") return undefined;
	return existingMemoryRoot(abs);
}

function isRunsPath(p: string): boolean {
	return basename(p) === ".runs" || p.includes(`${sep}.runs${sep}`);
}

export function seedFromParentMemory(parent: string, root: string): void {
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
