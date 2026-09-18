import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { resolveJevApiKey } from "../config.js";
import { assignObservationTimestamps } from "../ids.js";
import { extractCandidates } from "../jev/candidates.js";
import { JevClient } from "../jev/client.js";
import { observeChunk } from "../jev/observe.js";
import type { JevAsker } from "../jev/types.js";
import {
	entryIndexForId,
	foldLedger,
	latestCoverageMarkerId,
	rawTokensAfterIndex,
	selectSourceSlice,
	serializeSourceAddressedBranchEntries,
	OM_COST,
	OM_OBSERVATIONS_RECORDED,
	type Entry,
	type SourceSlice,
} from "../ledger/index.js";
import type { Runtime } from "../runtime.js";

type TriggerCtx = {
	cwd?: string;
	hasUI: boolean;
	ui?: { notify: (message: string, level?: "info" | "warning" | "error") => void };
	sessionManager: { getBranch: () => Entry[]; getEntries: () => Entry[] };
	getContextUsage?: () => { tokens: number | null } | undefined;
};

let runCounter = 0;

export function recordWorkerCost(
	pi: ExtensionAPI,
	runtime: Runtime,
	ctx: { sessionManager: { getEntries: () => Entry[] } },
	role: "observer" | "consolidator",
	runId: string,
	costUsd = 0,
	requests = 0,
): void {
	pi.appendEntry(OM_COST, { costUsd, role, runId, requests });
	runtime.refreshCost(ctx.sessionManager.getEntries());
}

function nextRunId(): string {
	runCounter += 1;
	const stamp = new Date().toISOString().replace(/[-:.TZ]/g, "").slice(0, 14);
	return `obs-${stamp}-${process.pid}-${runCounter}`;
}

function laterMarkerId(branch: Entry[], a: string | undefined, b: string | undefined): string | undefined {
	const ia = entryIndexForId(branch, a);
	const ib = entryIndexForId(branch, b);
	if (ia < 0 && ib < 0) return undefined;
	return ia >= ib ? a : b;
}

function effectiveWatermarkId(runtime: Runtime, branch: Entry[]): string | undefined {
	const committed = latestCoverageMarkerId(branch, OM_OBSERVATIONS_RECORDED);
	const dispatchedResolved =
		entryIndexForId(branch, runtime.dispatchedCoversUpToId) >= 0 ? runtime.dispatchedCoversUpToId : undefined;
	return laterMarkerId(branch, committed, dispatchedResolved);
}

export function createAsker(runtime: Runtime): JevAsker {
	if (runtime.asker) return runtime.asker;
	return new JevClient({
		apiKey: resolveJevApiKey(runtime.config),
		model: runtime.config.jev.model,
		baseUrl: runtime.config.jev.baseUrl,
	});
}

export function evaluateObserverTriggers(pi: ExtensionAPI, runtime: Runtime, ctx: TriggerCtx): void {
	if (!runtime.enabled || runtime.config.passive) return;

	const startToastLines: string[] = [];

	while (runtime.observerSlotsAvailable > 0) {
		const branch = ctx.sessionManager.getBranch();
		const watermarkId = effectiveWatermarkId(runtime, branch);
		const watermarkIndex = entryIndexForId(branch, watermarkId);
		const remaining = rawTokensAfterIndex(branch, watermarkIndex);
		if (remaining < runtime.config.chunkTokens) break;

		const slice = selectSourceSlice(branch, watermarkId, runtime.config.chunkTokens);
		if (slice.entries.length === 0 || !slice.coversUpToId) break;

		runtime.dispatchedCoversUpToId = slice.coversUpToId;
		runtime.trackObserverTask(dispatchObserver(pi, runtime, ctx, slice));
		if (ctx.hasUI) startToastLines.push(`om: observer started (~${slice.tokens.toLocaleString()} tok)`);
	}

	if (startToastLines.length > 0) ctx.ui?.notify(startToastLines.join("\n"), "info");
	runtime.refreshFooterGauges(ctx.sessionManager.getBranch(), ctx.getContextUsage?.()?.tokens ?? null);
}

async function dispatchObserver(
	pi: ExtensionAPI,
	runtime: Runtime,
	ctx: TriggerCtx,
	slice: SourceSlice,
): Promise<void> {
	const runId = nextRunId();
	const controller = new AbortController();
	const coversUpToId = slice.coversUpToId!;
	runtime.observersInFlight.set(runId, { controller, coversUpToId });
	const lastEntry = slice.entries.at(-1);
	runtime.status.workerStart("observer", runId);

	try {
		const { text: chunkText } = serializeSourceAddressedBranchEntries(slice.entries);
		const candidates = extractCandidates(slice.entries, runtime.config.jev.maxCandidates);
		const result = await observeChunk(
			chunkText,
			candidates,
			createAsker(runtime),
			{
				keepThreshold: runtime.config.jev.keepThreshold,
				maxStateTokens: runtime.config.jev.maxStateTokens,
				maxRequestTokens: runtime.config.jev.maxRequestTokens,
			},
			controller.signal,
		);

		const branch = ctx.sessionManager.getBranch();
		const used = foldLedger(branch).observationsByTimestamp.keys();
		const observations = assignObservationTimestamps(result.accepted, {
			used,
			fallbackAnchor: lastEntry?.timestamp,
		});

		pi.appendEntry(OM_OBSERVATIONS_RECORDED, { observations, coversUpToId });
		recordWorkerCost(pi, runtime, ctx, "observer", runId, 0, result.requests);
		runtime.status.workerDone(runId, observations.length);
		runtime.refreshFooterGauges(ctx.sessionManager.getBranch(), ctx.getContextUsage?.()?.tokens ?? null);
		if (ctx.hasUI && ctx.ui) {
			runtime.queueToast(
				`om: observer +${observations.length} (~${slice.tokens.toLocaleString()} tok)`,
				"info",
				ctx.ui.notify.bind(ctx.ui),
			);
		}
	} catch (error) {
		if (controller.signal.aborted) return;
		const message = error instanceof Error ? error.message : String(error);
		runtime.lastWorkerError = message;
		runtime.status.workerError(runId);
		if (ctx.hasUI) ctx.ui?.notify(`om: observer failed: ${message}`, "error");
	} finally {
		runtime.observersInFlight.delete(runId);
	}
}

export function registerObserverTrigger(pi: ExtensionAPI, runtime: Runtime): void {
	const handler = (_event: unknown, ctx: TriggerCtx) => evaluateObserverTriggers(pi, runtime, ctx);
	pi.on("turn_end", handler as never);
	pi.on("agent_start", handler as never);
}
