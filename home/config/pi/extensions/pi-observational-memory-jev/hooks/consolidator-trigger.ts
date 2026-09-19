import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { join } from "node:path";
import { consolidateOverflow, renderTopicFile } from "../jev/consolidate.js";
import {
	OM_OBSERVATIONS_DROPPED,
	foldLedger,
	lastSourceEntryId,
	nowTimestamp,
	poolTokens,
	selectPromotionOverflow,
	type Entry,
} from "../ledger/index.js";
import { renderIndexFile } from "../memory/index-render.js";
import { atomicWrite, indexPath, journeyPath, listTopics, readJourney, topicBodies } from "../memory/paths.js";
import type { Runtime } from "../runtime.js";
import { createAsker, recordWorkerCost } from "./observer-trigger.js";

type TriggerCtx = {
	hasUI: boolean;
	ui?: { notify: (message: string, level?: "info" | "warning" | "error") => void };
	sessionManager: { getBranch: () => Entry[]; getEntries: () => Entry[] };
	getContextUsage?: () => { tokens: number | null } | undefined;
};

let runCounter = 0;

function nextRunId(): string {
	runCounter += 1;
	const stamp = new Date().toISOString().replace(/[-:.TZ]/g, "").slice(0, 14);
	return `cons-${stamp}-${process.pid}-${runCounter}`;
}

export function evaluateConsolidatorTrigger(pi: ExtensionAPI, runtime: Runtime, ctx: TriggerCtx): void {
	if (!runtime.enabled || runtime.config.passive || runtime.consolidatorInFlight) return;

	const branch = ctx.sessionManager.getBranch();
	const active = foldLedger(branch).activeObservations;
	if (poolTokens(active) < runtime.config.consolidateAtPoolTokens) return;

	const { promote } = selectPromotionOverflow(active, runtime.config.poolTargetTokens);
	if (promote.length === 0) return;

	const runId = nextRunId();
	const controller = new AbortController();
	runtime.consolidatorInFlight = true;
	runtime.consolidatorController = controller;
	runtime.status.workerStart("consolidator", runId);
	if (ctx.hasUI) ctx.ui?.notify(`om: consolidator started (${promote.length} observations)`, "info");

	void (async () => {
		try {
			const result = await consolidateOverflow(
				promote,
				topicBodies(runtime.memoryRoot),
				readJourney(runtime.memoryRoot),
				createAsker(runtime),
				{
					keepThreshold: runtime.config.jev.keepThreshold,
					journeyTargetTokens: runtime.config.journeyTargetTokens,
					updated: nowTimestamp(),
				},
				controller.signal,
			);

			for (const write of result.writes) {
				atomicWrite(join(runtime.memoryRoot, write.filename), renderTopicFile(write));
			}
			if (result.journey) atomicWrite(journeyPath(runtime.memoryRoot), `${result.journey}\n`);
			atomicWrite(indexPath(runtime.memoryRoot), renderIndexFile(listTopics(runtime.memoryRoot)));

			const coversUpToId = lastSourceEntryId(ctx.sessionManager.getBranch());
			if (result.droppedTimestamps.length > 0 && coversUpToId) {
				pi.appendEntry(OM_OBSERVATIONS_DROPPED, {
					observationTimestamps: result.droppedTimestamps,
					coversUpToId,
				});
			}
			recordWorkerCost(pi, runtime, ctx, "consolidator", runId, {
				requests: result.requests,
				inputTokens: result.inputTokens,
				outputTokens: result.outputTokens,
			});
			runtime.status.workerDone(runId, result.writes.length);
			runtime.refreshFooterGauges(ctx.sessionManager.getBranch(), ctx.getContextUsage?.()?.tokens ?? null);
			if (ctx.hasUI) {
				runtime.queueToast(
					`om: consolidator wrote ${result.writes.length} topic file(s)`,
					"info",
					ctx.ui!.notify.bind(ctx.ui),
				);
			}
		} catch (error) {
			if (controller.signal.aborted) return;
			const message = error instanceof Error ? error.message : String(error);
			runtime.lastWorkerError = message;
			runtime.status.workerError(runId);
			if (ctx.hasUI) ctx.ui?.notify(`om: consolidator failed: ${message}`, "error");
		} finally {
			runtime.consolidatorInFlight = false;
			runtime.consolidatorController = undefined;
		}
	})();
}

export function registerConsolidatorTrigger(pi: ExtensionAPI, runtime: Runtime): void {
	const handler = (_event: unknown, ctx: TriggerCtx) => evaluateConsolidatorTrigger(pi, runtime, ctx);
	pi.on("turn_end", handler as never);
	pi.on("agent_start", handler as never);
}
