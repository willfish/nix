function pad(n: number): string {
	return n.toString().padStart(2, "0");
}

function fmtLocal(d: Date): string {
	return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

export function formatTimestamp(v: number | string | undefined): string {
	if (v === undefined) return "????-??-?? ??:??";
	const d = new Date(v);
	return Number.isNaN(d.getTime()) ? "????-??-?? ??:??" : fmtLocal(d);
}

export function nowTimestamp(): string {
	return fmtLocal(new Date());
}

export const MAX_RECORD_CONTENT_CHARS = 10_000;

export function truncateRecordContent(content: string): string {
	if (content.length <= MAX_RECORD_CONTENT_CHARS) return content;
	const head = content.slice(0, MAX_RECORD_CONTENT_CHARS);
	const dropped = content.length - MAX_RECORD_CONTENT_CHARS;
	return `${head} … [truncated ${dropped} chars]`;
}

function textAndPlaceholders(content: unknown): string {
	if (typeof content === "string") return content;
	if (!Array.isArray(content)) return "[non-text content omitted]";

	const parts: string[] = [];
	for (const block of content as Array<Record<string, unknown>>) {
		if (!block || typeof block !== "object") {
			parts.push("[non-text content omitted]");
			continue;
		}
		if (block.type === "text" && typeof block.text === "string") {
			parts.push(block.text);
			continue;
		}
		if (block.type === "thinking") {
			parts.push("[thinking omitted]");
			continue;
		}
		if (block.type === "toolCall" && typeof block.name === "string") {
			const args = JSON.stringify(block.arguments ?? {});
			parts.push(`[${block.name}(${truncateRecordContent(args)})]`);
			continue;
		}
		parts.push("[non-text content omitted]");
	}
	return parts.join("\n");
}

function textOnly(content: unknown): string {
	if (content == null) return "";
	if (typeof content === "string") return content;
	if (!Array.isArray(content)) return "";
	return content
		.filter((b): b is { type: string; text: string } => !!b && b.type === "text" && typeof b.text === "string")
		.map((b) => b.text)
		.join("\n");
}

export type RenderableEntry = {
	type: string;
	id?: string;
	timestamp?: string;
	message?: unknown;
	customType?: string;
	content?: unknown;
	summary?: unknown;
};

function renderMessage(message: unknown): string | null {
	if (!message || typeof message !== "object") return null;
	const msg = message as { role?: string; content?: unknown; timestamp?: number | string; toolName?: string };
	const time = formatTimestamp(msg.timestamp);
	if (msg.role === "user") return `[User @ ${time}]: ${textOnly(msg.content)}`;
	if (msg.role === "assistant") {
		const body = textAndPlaceholders(msg.content).split("\n").filter(Boolean).join("\n");
		if (!body) return null;
		return `[Assistant @ ${time}]: ${body}`;
	}
	const toolName = msg.toolName ?? "tool";
	return `[Tool result for ${toolName} @ ${time}]: ${textOnly(msg.content)}`;
}

function renderCustomMessage(entry: RenderableEntry): string {
	const time = formatTimestamp(entry.timestamp);
	const text = textOnly(entry.content) || (typeof entry.content === "string" ? entry.content : "");
	const tag = entry.customType ? `Custom (${entry.customType})` : "Custom";
	return `[${tag} @ ${time}]: ${text}`;
}

export function serializeBranchEntries(entries: RenderableEntry[]): string {
	const blocks: string[] = [];
	for (const entry of entries) {
		if (entry.type === "message" && entry.message) {
			const part = renderMessage(entry.message);
			if (part) blocks.push(part);
			continue;
		}
		if (entry.type === "custom_message") {
			blocks.push(renderCustomMessage(entry));
			continue;
		}
		if (entry.type === "branch_summary" && typeof entry.summary === "string") {
			const time = formatTimestamp(entry.timestamp);
			blocks.push(`[Branch summary @ ${time}]: ${entry.summary}`);
		}
	}
	return blocks.join("\n\n");
}

export type SourceAddressedSerialization = {
	text: string;
	sourceEntryIds: string[];
};

function isSourceRenderableEntry(entry: RenderableEntry): boolean {
	return entry.type === "message" || entry.type === "custom_message" || entry.type === "branch_summary";
}

export function serializeSourceAddressedBranchEntries(entries: RenderableEntry[]): SourceAddressedSerialization {
	const blocks: string[] = [];
	const sourceEntryIds: string[] = [];
	for (const entry of entries) {
		if (!entry.id || !isSourceRenderableEntry(entry)) continue;
		const rendered = serializeBranchEntries([entry]);
		if (!rendered.trim()) continue;
		sourceEntryIds.push(entry.id);
		blocks.push(`[Source entry id: ${entry.id}]\n${rendered}`);
	}
	return { text: blocks.join("\n\n"), sourceEntryIds };
}

export function extractEntryPlainText(entry: RenderableEntry): string {
	if (entry.type === "message" && entry.message) {
		const message = entry.message as { content?: unknown };
		return textAndPlaceholders(message.content);
	}
	if (entry.type === "custom_message") return textOnly(entry.content);
	if (entry.type === "branch_summary" && typeof entry.summary === "string") return entry.summary;
	return "";
}
