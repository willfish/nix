import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import {
	buildCompactionProjection,
	entryIndexById,
	isObservationsRecordedEntry,
	isSourceEntry,
	isValidCutPoint,
	rawTokensAfterIndex,
	renderSummary,
	type Entry,
} from "../ledger/index.js";
import { renderMemoryMap } from "../memory/index-render.js";
import { listTopics, readJourney } from "../memory/paths.js";
import type { Runtime } from "../runtime.js";

function chunkBoundaryIndices(branch: Entry[]): number[] {
	const indexes = entryIndexById(branch);
	const set = new Set<number>();
	for (const entry of branch) {
		if (!isObservationsRecordedEntry(entry)) continue;
		const idx = indexes.get(entry.data.coversUpToId);
		if (idx !== undefined) set.add(idx);
	}
	return Array.from(set).sort((a, b) => a - b);
}

function firstKeptAfterBoundary(branch: Entry[], boundaryIndex: number): Entry | undefined {
	for (let i = boundaryIndex + 1; i < branch.length; i++) {
		if (!isSourceEntry(branch[i])) continue;
		return isValidCutPoint(branch[i]) ? branch[i] : undefined;
	}
	return undefined;
}

export function snapCutoff(
	branch: Entry[],
	proposedFirstKeptId: string,
	tailTokens: number,
): { firstKeptId: string; tail: number | undefined } {
	const boundaries = chunkBoundaryIndices(branch);
	let bestId: string | undefined;
	let bestTail: number | undefined;
	let bestDelta = Number.POSITIVE_INFINITY;

	for (const boundaryIndex of boundaries) {
		const firstKept = firstKeptAfterBoundary(branch, boundaryIndex);
		if (!firstKept) continue;
		const tail = rawTokensAfterIndex(branch, boundaryIndex);
		const delta = Math.abs(tail - tailTokens);
		if (delta < bestDelta) {
			bestDelta = delta;
			bestId = firstKept.id;
			bestTail = tail;
		}
	}

	return bestId ? { firstKeptId: bestId, tail: bestTail } : { firstKeptId: proposedFirstKeptId, tail: undefined };
}

export function lastBranchEntryId(branch: Entry[]): string {
	for (let i = branch.length - 1; i >= 0; i--) {
		const id = branch[i]?.id;
		if (typeof id === "string" && id.length > 0) return id;
	}
	return "";
}

export function planOmCompaction(
	runtime: Runtime,
	branch: Entry[],
	proposedFirstKeptId: string,
): { firstKeptId: string; summary: string; details: ReturnType<typeof buildCompactionProjection>["details"] } | undefined {
	if (!proposedFirstKeptId) return undefined;
	const snap = snapCutoff(branch, proposedFirstKeptId, runtime.config.tailTokens);
	const projection = buildCompactionProjection(branch, snap.firstKeptId);
	const journey = readJourney(runtime.memoryRoot);
	const map = renderMemoryMap(listTopics(runtime.memoryRoot));
	const summary = renderSummary(journey, map, projection.observations);
	if (!summary) return undefined;
	return { firstKeptId: snap.firstKeptId, summary, details: projection.details };
}

export function canSkipObserverWait(
	branch: Entry[],
	snappedFirstKeptId: string,
	snappedTail: number | undefined,
	tailTokens: number,
	observersInFlight: Iterable<{ coversUpToId: string }>,
): boolean {
	if (snappedTail === undefined || snappedTail > tailTokens) return false;
	const indexes = entryIndexById(branch);
	const cutoffIndex = indexes.get(snappedFirstKeptId);
	if (cutoffIndex === undefined) return false;
	for (const { coversUpToId } of observersInFlight) {
		const idx = indexes.get(coversUpToId);
		if (idx === undefined || idx < cutoffIndex) return false;
	}
	return true;
}

export function registerCompactionHook(pi: ExtensionAPI, runtime: Runtime): void {
	pi.on("session_before_compact", async (event: any, ctx: any) => {
		if (!runtime.enabled || runtime.config.passive) return undefined;

		const hasUI = ctx.hasUI;
		if (runtime.compactHookInFlight) {
			if (hasUI) ctx.ui.notify("om: another compaction is already in progress; cancelling duplicate", "warning");
			return { cancel: true };
		}

		runtime.compactHookInFlight = true;
		try {
			runtime.ensureConfig(ctx.cwd);
			const tailTokens = runtime.config.tailTokens;
			const { firstKeptEntryId, tokensBefore } = event.preparation;

			let branch = (ctx.sessionManager?.getBranch?.() as Entry[] | undefined) ?? (event.branchEntries as Entry[]);
			let snap = snapCutoff(branch, firstKeptEntryId, tailTokens);

			const skip = canSkipObserverWait(
				branch,
				snap.firstKeptId,
				snap.tail,
				tailTokens,
				runtime.observersInFlight.values(),
			);
			runtime.lastCompactionObserverWait = skip ? "skipped" : "waited";
			if (!skip) {
				if (hasUI) ctx.ui.notify("om: waiting for in-flight observers before folding…", "info");
				await runtime.whenObserversIdle();
				branch = (ctx.sessionManager?.getBranch?.() as Entry[] | undefined) ?? (event.branchEntries as Entry[]);
				snap = snapCutoff(branch, firstKeptEntryId, tailTokens);
			}

			const planned = planOmCompaction(runtime, branch, snap.firstKeptId);
			if (!planned) return undefined;

			return {
				compaction: {
					summary: planned.summary,
					firstKeptEntryId: planned.firstKeptId,
					tokensBefore,
					details: planned.details,
				},
			};
		} finally {
			runtime.compactHookInFlight = false;
		}
	});
}
