import { cpSync, existsSync, renameSync, rmSync } from "node:fs";
import { basename, sep } from "node:path";

export function existingMemoryRoot(root: string | undefined): string | undefined {
	return root && existsSync(root) ? root : undefined;
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
