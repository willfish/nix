import { existsSync, mkdirSync, readFileSync, readdirSync, renameSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

export const INDEX_FILENAME = "INDEX.md";
export const JOURNEY_FILENAME = "JOURNEY.md";

export function memoryBaseDir(cwd: string): string {
	return join(cwd, ".memory");
}

export function sessionMemoryRoot(cwd: string, sessionId: string): string {
	return join(memoryBaseDir(cwd), sessionId);
}

export function indexPath(root: string): string {
	return join(root, INDEX_FILENAME);
}

export function journeyPath(root: string): string {
	return join(root, JOURNEY_FILENAME);
}

export function readJourney(root: string): string | undefined {
	const path = journeyPath(root);
	if (!existsSync(path)) return undefined;
	try {
		const body = readFileSync(path, "utf-8").trim();
		return body.length > 0 ? body : undefined;
	} catch {
		return undefined;
	}
}

export function atomicWrite(path: string, content: string): void {
	mkdirSync(dirname(path), { recursive: true });
	const tmp = `${path}.tmp-${process.pid}-${Date.now()}`;
	writeFileSync(tmp, content, "utf-8");
	renameSync(tmp, path);
}

export function resolveWithinMemory(root: string, requestedPath: string): string | undefined {
	const base = resolve(root);
	const abs = resolve(base, requestedPath);
	const rel = relative(base, abs);
	if (rel === "" || rel === ".") return abs;
	if (rel.startsWith("..") || resolve(base, rel) !== abs) return undefined;
	return abs;
}

export type TopicFrontMatter = {
	id?: string;
	title?: string;
	summary?: string;
	updated?: string;
};

export type Topic = TopicFrontMatter & {
	path: string;
	filename: string;
	body: string;
};

const FRONT_MATTER_RE = /^---\n([\s\S]*?)\n---\n?/;

export function parseFrontMatter(content: string): { front: TopicFrontMatter; body: string } {
	const match = FRONT_MATTER_RE.exec(content);
	if (!match) return { front: {}, body: content };
	const front: TopicFrontMatter = {};
	for (const line of match[1].split("\n")) {
		const idx = line.indexOf(":");
		if (idx < 0) continue;
		const key = line.slice(0, idx).trim();
		let value = line.slice(idx + 1).trim();
		if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
			value = value.slice(1, -1);
		}
		if (key === "id" || key === "title" || key === "summary" || key === "updated") {
			front[key] = value;
		}
	}
	return { front, body: content.slice(match[0].length) };
}

export function listTopics(root: string): Topic[] {
	if (!existsSync(root)) return [];
	const cwd = resolve(root, "..", "..");
	const topics: Topic[] = [];
	for (const filename of readdirSync(root)) {
		if (!filename.endsWith(".md") || filename === INDEX_FILENAME || filename === JOURNEY_FILENAME) continue;
		let content: string;
		try {
			content = readFileSync(join(root, filename), "utf-8");
		} catch {
			continue;
		}
		const { front, body } = parseFrontMatter(content);
		topics.push({ ...front, path: relative(cwd, join(root, filename)), filename, body });
	}
	topics.sort((a, b) => (a.filename < b.filename ? -1 : a.filename > b.filename ? 1 : 0));
	return topics;
}

export function topicBodies(root: string): Record<string, string> {
	const bodies: Record<string, string> = {};
	for (const topic of listTopics(root)) {
		bodies[topic.filename] = topic.body;
	}
	return bodies;
}
