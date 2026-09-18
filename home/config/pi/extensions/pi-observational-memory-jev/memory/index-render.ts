import type { Topic } from "./paths.js";

function summaryOf(topic: Topic): string {
	const s = (topic.summary ?? "").trim();
	return s.length > 0 ? s : "(no summary)";
}

function titleOf(topic: Topic): string {
	const t = (topic.title ?? "").trim();
	return t.length > 0 ? t : topic.filename;
}

export function renderIndexFile(topics: Topic[]): string {
	const parts: string[] = ["# Memory index", ""];
	if (topics.length === 0) {
		parts.push("_No topics yet._");
		return `${parts.join("\n")}\n`;
	}
	parts.push("Durable memory topics for this session. Read a file for its full current state.", "");
	for (const topic of topics) {
		const updated = topic.updated ? ` · updated ${topic.updated}` : "";
		parts.push(`## ${titleOf(topic)}`);
		parts.push(`- \`${topic.path}\`${updated}`);
		parts.push(`- ${summaryOf(topic)}`);
		parts.push("");
	}
	return `${parts.join("\n").trimEnd()}\n`;
}

export function renderMemoryMap(topics: Topic[]): string | undefined {
	if (topics.length === 0) return undefined;
	const lines: string[] = [
		"## Memory map",
		"Durable long-term notes live in `.memory/`. Read a file when a topic below looks relevant; these summaries are intentionally terse.",
	];
	for (const topic of topics) {
		const updated = topic.updated ? ` (updated ${topic.updated})` : "";
		lines.push(`- \`${topic.path}\` — ${summaryOf(topic)}${updated}`);
	}
	return lines.join("\n");
}
