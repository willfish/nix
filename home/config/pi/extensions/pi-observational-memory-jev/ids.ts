import { estimateStringTokens } from "./tokens.js";
import type { Observation, ObservationKind } from "./ledger/types.js";

export type ModelObservation = {
	timestamp: string;
	content: string;
	kind?: ObservationKind;
	keep?: number;
	sourceEntryId?: string;
};

const MODEL_TIMESTAMP_RE = /^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2})$/;

function pad(n: number): string {
	return n.toString().padStart(2, "0");
}

function formatAnchorBase(anchor: number | string | undefined): string | undefined {
	if (anchor === undefined) return undefined;
	const d = new Date(anchor);
	if (Number.isNaN(d.getTime())) return undefined;
	return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function modelTimestampToBase(modelTimestamp: string): string | undefined {
	const m = MODEL_TIMESTAMP_RE.exec(modelTimestamp.trim());
	if (!m) return undefined;
	return `${m[1]}T${m[2]}:00`;
}

export type AssignTimestampsOptions = {
	used?: Iterable<string>;
	fallbackAnchor?: number | string;
};

export function assignObservationTimestamps(
	modelObservations: ModelObservation[],
	options: AssignTimestampsOptions = {},
): Observation[] {
	const used = new Set<string>(options.used ?? []);
	const fallbackBase = formatAnchorBase(options.fallbackAnchor) ?? formatAnchorBase(Date.now())!;
	const result: Observation[] = [];

	for (const model of modelObservations) {
		const content = model.content.replace(/[\r\n]+/g, " ").trim();
		if (!content) continue;
		const base = modelTimestampToBase(model.timestamp) ?? fallbackBase;

		let timestamp = base;
		if (used.has(timestamp)) {
			let suffix = 1;
			do {
				timestamp = `${base}.${pad(suffix)}`;
				suffix++;
			} while (used.has(timestamp));
		}
		used.add(timestamp);

		const observation: Observation = { timestamp, content, tokenCount: estimateStringTokens(content) };
		if (model.kind) observation.kind = model.kind;
		if (typeof model.keep === "number") observation.keep = model.keep;
		if (model.sourceEntryId) observation.sourceEntryId = model.sourceEntryId;
		result.push(observation);
	}

	return result;
}
