/** Observer output committed by the orchestrator; the buffer tier. */
export const OM_OBSERVATIONS_RECORDED = "om.observations.recorded";
/** Promotion tombstones written after a successful consolidator run. */
export const OM_OBSERVATIONS_DROPPED = "om.observations.dropped";
/** Compaction details type stamped into the compaction entry's `details`. */
export const OM_FOLDED = "om.folded";
/** Per-session on/off gate state (default OFF). */
export const OM_ENABLED = "om.enabled";
/**
 * Per-worker cost record. Summed across the whole session (every branch) so spend
 * never rolls back under /tree.
 */
export const OM_COST = "om.cost";
/**
 * Hidden continuation after a mid-run compaction. Pi surfaces custom → user for the model.
 */
export const OM_RESUME = "om.resume";

export const OBSERVATION_KINDS = [
	"fact",
	"decision",
	"constraint",
	"question",
	"correction",
	"hypothesis",
] as const;

export type ObservationKind = (typeof OBSERVATION_KINDS)[number];

export type Entry = {
	type: string;
	id: string;
	timestamp?: string;
	message?: unknown;
	content?: unknown;
	customType?: string;
	summary?: unknown;
	fromId?: string;
	data?: unknown;
	details?: unknown;
	firstKeptEntryId?: string;
};

/**
 * One observation. `timestamp` is the orchestrator-assigned unique id.
 * `content` is a verbatim single-line excerpt (never model-rewritten).
 * `kind` / `keep` come from Jev; `tokenCount` is computed in code.
 */
export type Observation = {
	timestamp: string;
	content: string;
	tokenCount: number;
	kind?: ObservationKind;
	keep?: number;
	sourceEntryId?: string;
};

export type ObservationsRecordedEntryData = {
	observations: Observation[];
	coversUpToId: string;
};

export type ObservationsDroppedEntryData = {
	observationTimestamps: string[];
	coversUpToId: string;
};

export type CostEntryData = {
	costUsd: number;
	role: "observer" | "consolidator";
	runId: string;
	requests?: number;
};

export type MemoryDetails = {
	type: typeof OM_FOLDED;
	version: 1;
	observations: Observation[];
};

export function isNonEmptyString(value: unknown): value is string {
	return typeof value === "string" && value.length > 0;
}

export function isNonEmptyStringArray(value: unknown): value is string[] {
	return Array.isArray(value) && value.length > 0 && value.every(isNonEmptyString);
}

function isTokenCount(value: unknown): value is number {
	return typeof value === "number" && Number.isFinite(value) && value >= 0;
}

function isPlainRecord(value: unknown): value is Record<string, unknown> {
	return !!value && typeof value === "object";
}

export function isObservationKind(value: unknown): value is ObservationKind {
	return typeof value === "string" && (OBSERVATION_KINDS as readonly string[]).includes(value);
}

export function isObservation(value: unknown): value is Observation {
	if (!isPlainRecord(value)) return false;
	if (
		!isNonEmptyString(value.timestamp) ||
		!isNonEmptyString(value.content) ||
		/\r|\n/.test(value.content) ||
		!isTokenCount(value.tokenCount)
	) {
		return false;
	}
	if (value.kind !== undefined && !isObservationKind(value.kind)) return false;
	if (value.keep !== undefined && (typeof value.keep !== "number" || !Number.isFinite(value.keep))) return false;
	if (value.sourceEntryId !== undefined && !isNonEmptyString(value.sourceEntryId)) return false;
	return true;
}

export function isObservationsRecordedData(value: unknown): value is ObservationsRecordedEntryData {
	if (!isPlainRecord(value)) return false;
	return (
		Array.isArray(value.observations) &&
		value.observations.every(isObservation) &&
		isNonEmptyString(value.coversUpToId)
	);
}

export function isObservationsDroppedData(value: unknown): value is ObservationsDroppedEntryData {
	if (!isPlainRecord(value)) return false;
	return isNonEmptyStringArray(value.observationTimestamps) && isNonEmptyString(value.coversUpToId);
}

export function isMemoryDetails(value: unknown): value is MemoryDetails {
	if (!isPlainRecord(value)) return false;
	return (
		value.type === OM_FOLDED &&
		value.version === 1 &&
		Array.isArray(value.observations) &&
		value.observations.every(isObservation)
	);
}

export function isObservationsRecordedEntry(entry: Entry): entry is Entry & {
	type: "custom";
	customType: typeof OM_OBSERVATIONS_RECORDED;
	data: ObservationsRecordedEntryData;
} {
	return entry.type === "custom" && entry.customType === OM_OBSERVATIONS_RECORDED && isObservationsRecordedData(entry.data);
}

export function isCostEntry(entry: Entry): entry is Entry & {
	type: "custom";
	customType: typeof OM_COST;
	data: CostEntryData;
} {
	if (entry.type !== "custom" || entry.customType !== OM_COST) return false;
	const data = entry.data as Record<string, unknown> | undefined;
	return !!data && typeof data.costUsd === "number" && Number.isFinite(data.costUsd) && data.costUsd >= 0;
}

export function sumSessionCost(allEntries: Entry[]): { costUsd: number; runs: number; requests: number } {
	let costUsd = 0;
	let runs = 0;
	let requests = 0;
	for (const entry of allEntries) {
		if (isCostEntry(entry)) {
			costUsd += entry.data.costUsd;
			runs += 1;
			requests += entry.data.requests ?? 0;
		}
	}
	return { costUsd, runs, requests };
}

export function isObservationsDroppedEntry(entry: Entry): entry is Entry & {
	type: "custom";
	customType: typeof OM_OBSERVATIONS_DROPPED;
	data: ObservationsDroppedEntryData;
} {
	return entry.type === "custom" && entry.customType === OM_OBSERVATIONS_DROPPED && isObservationsDroppedData(entry.data);
}
