/** Rough char-based token estimate (≈4 chars/token). */
export function estimateStringTokens(text: string): number {
	return Math.ceil(text.length / 4);
}

function textFromUnknown(value: unknown): string {
	if (typeof value === "string") return value;
	if (!Array.isArray(value)) return "";
	const parts: string[] = [];
	for (const block of value) {
		if (!block || typeof block !== "object") continue;
		const record = block as Record<string, unknown>;
		if (record.type === "text" && typeof record.text === "string") parts.push(record.text);
	}
	return parts.join("\n");
}

/**
 * Token cost of one ledger entry. Only source entries (message / custom_message /
 * branch_summary) count; ledger and compaction records are 0 so clocks measure new conversation.
 */
export function estimateEntryTokens(entry: {
	type: string;
	message?: unknown;
	content?: unknown;
	summary?: unknown;
}): number {
	if (entry.type === "message" && entry.message) {
		const message = entry.message as { content?: unknown };
		return estimateStringTokens(textFromUnknown(message.content));
	}
	if (entry.type === "custom_message" && entry.content) {
		return estimateStringTokens(textFromUnknown(entry.content));
	}
	if (entry.type === "branch_summary" && typeof entry.summary === "string") {
		return estimateStringTokens(entry.summary);
	}
	return 0;
}
