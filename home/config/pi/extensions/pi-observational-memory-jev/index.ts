/**
 * Observational memory with Jev as the observation backend.
 *
 * Same /om ergonomics as Eero Alvar's observational-memory extension: per-session
 * gate (package default off; `enabledByDefault` can turn new sessions on), parallel
 * observers, branch-local ledger, deterministic compaction, consolidator into
 * `.memory/<sessionId>/`. Observers call TypeSafe Jev instead of a chat-model pi
 * subprocess, so observations stay verbatim excerpts with typed scores.
 */
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { registerCompactCommand } from "./commands/compact.js";
import { registerConsolidateCommand } from "./commands/consolidate.js";
import { registerStatusCommand } from "./commands/status.js";
import { initialOmEnabled, readEnabledFromLedger } from "./gate.js";
import { registerCompactionHook } from "./hooks/compaction-hook.js";
import { registerCompactionTrigger } from "./hooks/compaction-trigger.js";
import { registerConsolidatorTrigger } from "./hooks/consolidator-trigger.js";
import { registerObserverTrigger } from "./hooks/observer-trigger.js";
import { OM_ENABLED, type Entry } from "./ledger/index.js";
import { ensureSessionMemory } from "./memory/session.js";
import { Runtime } from "./runtime.js";
import { shouldAttachOmStatus } from "./spend.js";

export default function observationalMemoryJev(pi: ExtensionAPI): void {
	const runtime = new Runtime();

	function attachIfEnabled(ctx: any): void {
		if (runtime.enabled && shouldAttachOmStatus(ctx)) {
			runtime.status.attach(ctx.ui);
		} else {
			runtime.status.detach();
		}
	}

	pi.on("session_start", (_event: unknown, ctx: any) => {
		runtime.ensureConfig(ctx.cwd);
		runtime.dispatchedCoversUpToId = undefined;
		const branch = ctx.sessionManager.getBranch() as Entry[];
		const ledgerGate = readEnabledFromLedger(branch);
		runtime.enabled = initialOmEnabled(branch, runtime.config.enabledByDefault);
		if (ledgerGate === undefined && runtime.enabled) {
			pi.appendEntry(OM_ENABLED, { enabled: true });
		}
		if (runtime.enabled) runtime.memoryRoot = ensureSessionMemory(ctx);
		attachIfEnabled(ctx);
		runtime.refreshFooterGauges(branch, ctx.getContextUsage?.()?.tokens ?? null);
		runtime.refreshCost(ctx.sessionManager.getEntries() as Entry[]);
	});

	pi.on("session_shutdown", () => {
		runtime.status.detach();
		runtime.abortAllWorkers();
	});

	pi.registerCommand("om", {
		description: "Toggle observational memory for this session (/om on, /om off)",
		handler: async (args: string, ctx: any) => {
			const arg = (args ?? "").trim().toLowerCase();
			const next = arg === "on" ? true : arg === "off" ? false : !runtime.enabled;
			if (next === runtime.enabled) {
				if (ctx.hasUI) ctx.ui.notify(`om already ${next ? "on" : "off"}`, "info");
				return;
			}
			runtime.enabled = next;
			pi.appendEntry(OM_ENABLED, { enabled: next });
			if (next) {
				runtime.memoryRoot = ensureSessionMemory(ctx);
				attachIfEnabled(ctx);
				runtime.refreshFooterGauges(ctx.sessionManager.getBranch() as Entry[], ctx.getContextUsage?.()?.tokens ?? null);
				runtime.refreshCost(ctx.sessionManager.getEntries() as Entry[]);
			} else {
				runtime.abortAllWorkers();
				runtime.status.detach();
			}
			if (ctx.hasUI) ctx.ui.notify(`om ${next ? "enabled" : "disabled"}`, "info");
		},
	});

	registerObserverTrigger(pi, runtime);
	registerConsolidatorTrigger(pi, runtime);
	registerCompactionTrigger(pi, runtime);
	registerCompactionHook(pi, runtime);

	registerStatusCommand(pi, runtime);
	registerCompactCommand(pi, runtime);
	registerConsolidateCommand(pi, runtime);
}
