import { type Config, DEFAULTS, loadConfig } from "./config.js";
import type { JevAsker } from "./jev/types.js";
import { foldLedger, poolTokens, rawTokensSinceObservationCoverage, sumSessionCost, type Entry } from "./ledger/index.js";
import { StatusController } from "./ui/status-controller.js";

export class Runtime {
	config: Config = { ...DEFAULTS, jev: { ...DEFAULTS.jev } };
	configLoaded = false;
	enabled = false;
	memoryRoot = "";
	asker: JevAsker | undefined;

	readonly observersInFlight = new Map<string, { controller: AbortController; coversUpToId: string }>();
	readonly observerTasks = new Set<Promise<void>>();

	consolidatorInFlight = false;
	consolidatorController: AbortController | undefined;

	dispatchedCoversUpToId: string | undefined;

	compactInFlight = false;
	compactHookInFlight = false;

	lastWorkerError: string | undefined;
	lastCompactionObserverWait: "skipped" | "waited" | undefined;

	readonly status = new StatusController();

	private pendingInfoToastLines: string[] = [];
	private infoToastFlushTimer: ReturnType<typeof setTimeout> | undefined;

	queueToast(
		line: string,
		level: "info" | "warning" | "error",
		notify: (message: string, level: "info" | "warning" | "error") => void,
	): void {
		if (level !== "info") {
			notify(line, level);
			return;
		}
		this.pendingInfoToastLines.push(line);
		if (this.infoToastFlushTimer !== undefined) return;
		this.infoToastFlushTimer = setTimeout(() => {
			this.infoToastFlushTimer = undefined;
			const lines = this.pendingInfoToastLines.splice(0);
			if (lines.length > 0) notify(lines.join("\n"), "info");
		}, 0);
		this.infoToastFlushTimer.unref?.();
	}

	cancelPendingToasts(): void {
		if (this.infoToastFlushTimer !== undefined) {
			clearTimeout(this.infoToastFlushTimer);
			this.infoToastFlushTimer = undefined;
		}
		this.pendingInfoToastLines = [];
	}

	ensureConfig(cwd: string): void {
		if (this.configLoaded) return;
		this.config = loadConfig(cwd);
		this.configLoaded = true;
	}

	refreshFooterGauges(branch: Entry[], contextTokens?: number | null): void {
		if (!this.enabled) return;
		const folded = foldLedger(branch);
		this.status.setGauges({
			nextValue: rawTokensSinceObservationCoverage(branch),
			nextMax: this.config.chunkTokens,
			poolValue: poolTokens(folded.activeObservations),
			poolMax: this.config.consolidateAtPoolTokens,
			ctxValue: contextTokens ?? 0,
			ctxMax: this.config.compactAtContextTokens,
		});
	}

	refreshCost(allEntries: Entry[]): void {
		if (!this.enabled) return;
		this.status.setSpend(sumSessionCost(allEntries));
	}

	abortAllWorkers(): void {
		this.cancelPendingToasts();
		for (const { controller } of this.observersInFlight.values()) {
			controller.abort();
		}
		this.observersInFlight.clear();
		this.consolidatorController?.abort();
		this.consolidatorController = undefined;
		this.consolidatorInFlight = false;
	}

	trackObserverTask(task: Promise<void>): void {
		this.observerTasks.add(task);
		void task.finally(() => this.observerTasks.delete(task));
	}

	async whenObserversIdle(): Promise<void> {
		while (this.observerTasks.size > 0) {
			await Promise.allSettled([...this.observerTasks]);
		}
	}

	get observerSlotsAvailable(): number {
		return Math.max(0, this.config.observerConcurrency - this.observersInFlight.size);
	}
}
