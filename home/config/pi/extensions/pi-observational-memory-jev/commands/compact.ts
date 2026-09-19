import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { lastBranchEntryId, planOmCompaction } from "../hooks/compaction-hook.js";
import type { Entry } from "../ledger/index.js";
import { isPiCompactTooSmall } from "../spend.js";
import type { Runtime } from "../runtime.js";

function clipSummary(summary: string, max = 4000): string {
	if (summary.length <= max) return summary;
	return `${summary.slice(0, max - 1)}…`;
}

export function formatSmallSessionCompact(summary: string | undefined): string {
	if (!summary) return "om: nothing to compact (empty ledger)";
	return `om: compaction complete (session too small to cut)\n${clipSummary(summary)}`;
}

export function registerCompactCommand(pi: ExtensionAPI, runtime: Runtime): void {
	pi.registerCommand("om:compact", {
		description: "Force observational-memory compaction now (renders the ledger even if Pi will not cut a small session)",
		handler: async (_args: string, ctx: any) => {
			if (!runtime.enabled) {
				if (ctx.hasUI) ctx.ui.notify("om is off (use /om on to enable)", "info");
				return;
			}
			if (runtime.compactInFlight) {
				if (ctx.hasUI) ctx.ui.notify("om: compaction already in progress", "warning");
				return;
			}
			runtime.compactInFlight = true;
			if (ctx.hasUI) ctx.ui.notify("om: compacting (waiting for in-flight observers)…", "info");
			await runtime.whenObserversIdle();
			runtime.ensureConfig(ctx.cwd);
			const branch = ctx.sessionManager.getBranch() as Entry[];
			const planned = planOmCompaction(runtime, branch, lastBranchEntryId(branch));

			ctx.compact({
				onComplete: () => {
					runtime.compactInFlight = false;
					if (ctx.hasUI) ctx.ui.notify("om: compaction complete", "info");
				},
				onError: (error: { message: string }) => {
					runtime.compactInFlight = false;
					if (error.message === "Compaction cancelled") return;
					if (isPiCompactTooSmall(error.message)) {
						if (ctx.hasUI) ctx.ui.notify(formatSmallSessionCompact(planned?.summary), "info");
						return;
					}
					if (ctx.hasUI) ctx.ui.notify(`om: ${error.message}`, "error");
				},
			});
		},
	});
}
