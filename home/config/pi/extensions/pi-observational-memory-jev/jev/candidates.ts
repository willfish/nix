import { formatTimestamp, extractEntryPlainText, type RenderableEntry } from "../ledger/serialize.js";

export type Candidate = {
	id: string;
	sourceEntryId: string;
	timestamp: string;
	text: string;
};

const MIN_CHARS = 24;
const MAX_CHARS = 500;

function splitBlocks(text: string): string[] {
	return text
		.split(/\n{2,}|(?<=[.!?])\s+(?=[A-Z([{])/)
		.map((part) => part.replace(/\s+/g, " ").trim())
		.filter((part) => part.length >= MIN_CHARS)
		.map((part) => (part.length <= MAX_CHARS ? part : `${part.slice(0, MAX_CHARS - 1)}…`));
}

function entryTimestamp(entry: RenderableEntry): string {
	if (entry.type === "message" && entry.message && typeof entry.message === "object") {
		const message = entry.message as { timestamp?: number | string };
		return formatTimestamp(message.timestamp ?? entry.timestamp);
	}
	return formatTimestamp(entry.timestamp);
}

/**
 * Cut a source slice into labelled verbatim candidates. Jev never rewrites these strings;
 * it only scores them.
 */
export function extractCandidates(entries: RenderableEntry[], maxCandidates: number): Candidate[] {
	const candidates: Candidate[] = [];
	for (const entry of entries) {
		if (!entry.id) continue;
		const plain = extractEntryPlainText(entry).trim();
		if (!plain) continue;
		const timestamp = entryTimestamp(entry);
		const blocks = splitBlocks(plain);
		const parts = blocks.length > 0 ? blocks : [plain.replace(/\s+/g, " ").trim()].filter((p) => p.length >= MIN_CHARS);
		for (const text of parts) {
			candidates.push({
				id: `c${candidates.length + 1}`,
				sourceEntryId: entry.id,
				timestamp,
				text,
			});
		}
	}
	if (candidates.length <= maxCandidates) return candidates;
	return candidates.slice(0, maxCandidates);
}
