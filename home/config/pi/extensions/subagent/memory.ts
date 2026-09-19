import { existsSync, readFileSync } from "node:fs";
import { basename, dirname, join, relative, resolve } from "node:path";

function readOptionalMarkdown(path: string): string {
	if (!existsSync(path)) return "";
	try {
		return readFileSync(path, "utf8").trim();
	} catch {
		return "";
	}
}

/** Parent `.memory/<sessionId>` only. Rejects path-shaped session ids. */
export function parentObservationalMemoryRoot(cwd: unknown, sessionId: unknown): string | undefined {
	if (typeof cwd !== "string" || !cwd.trim() || typeof sessionId !== "string" || !sessionId.trim()) {
		return undefined;
	}
	if (/[\\/]|\.\.|\0/.test(sessionId)) return undefined;
	const abs = resolve(join(cwd, ".memory", sessionId));
	const rel = relative(resolve(cwd, ".memory"), abs);
	if (!rel || rel.startsWith("..")) return undefined;
	return existsSync(abs) ? abs : undefined;
}

export function observationalMemoryBriefing(root: string | undefined): string {
	if (!root || basename(dirname(root)) !== ".memory" || !existsSync(root)) return "";
	const journey = readOptionalMarkdown(join(root, "JOURNEY.md"));
	const index = readOptionalMarkdown(join(root, "INDEX.md"));
	if (!journey && !index) return "";
	const parts = [
		"## Parent observational memory",
		`Durable notes from the coordinator session are at ${root}. Read a topic file when a path below looks relevant.`,
	];
	if (journey) parts.push("### Journey", journey);
	if (index) parts.push(index);
	return parts.join("\n\n");
}

export function composeChildSystemPrompt(rolePrompt: string | undefined, memoryBriefing: string | undefined): string {
	return [rolePrompt, memoryBriefing].filter((part) => typeof part === "string" && part.trim()).join("\n\n");
}
